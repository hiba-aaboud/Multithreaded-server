#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>

#include "babble_server.h"
#include "babble_types.h"
#include "babble_utils.h"
#include "babble_communication.h"
#include "babble_server_answer.h"
#include "babble_registration.h"
#include "fastrand.h"
#include "babble_config.h"

static command_t *cmd_buff[BABBLE_BUFFER_SIZE];
static int buff_start = 0;
static int buff_end = 0;


pthread_mutex_t buff_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buff_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buff_not_full = PTHREAD_COND_INITIALIZER;

pthread_t exec_threads[BABBLE_EXECUTOR_THREADS];

int random_delay_activated = 0;

static void display_help(char *exec)
{
    printf("Usage: %s -p port_number -r [activate_random_delays]\n", exec);
}

static int parse_command(char *str, command_t *cmd)
{
    char *name = NULL;

    /* start by cleaning the input */
    str_clean(str);

    /* get command id */
    cmd->cid = str_to_command(str, &cmd->answer_expected);

    switch (cmd->cid)
    {
    case LOGIN:
        if (str_to_payload(str, cmd->msg, BABBLE_ID_SIZE))
        {
            name = get_name_from_key(cmd->key);
            fprintf(stderr, "Error from [%s]-- invalid LOGIN -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case PUBLISH:
        if (str_to_payload(str, cmd->msg, BABBLE_PUBLICATION_SIZE))
        {
            name = get_name_from_key(cmd->key);
            fprintf(stderr, "Warning from [%s]-- invalid PUBLISH -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case FOLLOW:
        if (str_to_payload(str, cmd->msg, BABBLE_ID_SIZE))
        {
            name = get_name_from_key(cmd->key);
            fprintf(stderr, "Warning from [%s]-- invalid FOLLOW -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case TIMELINE:
        cmd->msg[0] = '\0';
        break;
    case FOLLOW_COUNT:
        cmd->msg[0] = '\0';
        break;
    case RDV:
        cmd->msg[0] = '\0';
        break;
    default:
        name = get_name_from_key(cmd->key);
        fprintf(stderr, "Error from [%s]-- invalid client command -> %s\n", name, str);
        free(name);
        return -1;
    }

    return 0;
}

/* processes the command and eventually generates an answer */
static int process_command(command_t *cmd, answer_t **answer)
{
    int res = 0;

    switch (cmd->cid)
    {
    case LOGIN:
        res = run_login_command(cmd, answer);
        break;
    case PUBLISH:
        random_delay(random_delay_activated);
        res = run_publish_command(cmd, answer);
        break;
    case FOLLOW:
        random_delay(random_delay_activated);
        res = run_follow_command(cmd, answer);
        break;
    case TIMELINE:
        random_delay(random_delay_activated);
        res = run_timeline_command(cmd, answer);
        break;
    case FOLLOW_COUNT:
        res = run_fcount_command(cmd, answer);
        break;
    case RDV:
        res = run_rdv_command(cmd, answer);
        break;
    case UNREGISTER:
        res = unregisted_client(cmd);
        *answer = NULL;
        break;
    default:
        fprintf(stderr, "Error -- Unknown command id\n");
        return -1;
    }

    if (res)
    {
        fprintf(stderr, "Error -- Failed to run command ");
        display_command(cmd, stderr);
    }

    return res;
}

// buff opps
void add_to_buff(command_t *cmd)
{
    cmd_buff[buff_end] = cmd;
    buff_end = (buff_end + 1) % BABBLE_BUFFER_SIZE;
}

command_t *rmv_from_buff()
{
    command_t *cmd = cmd_buff[buff_start];
    buff_start = (buff_start + 1) % BABBLE_BUFFER_SIZE;
    return cmd;
}

int is_buff_full()
{
    return ((buff_end + 1) % BABBLE_BUFFER_SIZE) == buff_start;
}

int is_buffer_empty()
{
    return buff_start == buff_end;
}

// comm thread func
void *communication_thread(void *arg)
{
    int sockfd = *(int *)arg;
    free(arg);
    char *recv_buff = NULL;
    command_t *cmd;
    answer_t *answer = NULL;
    unsigned long cl_key = 0;
    char client_name[BABBLE_ID_SIZE + 1];

    memset(client_name, 0, BABBLE_ID_SIZE + 1);
    if (network_recv(sockfd, (void **)&recv_buff) > 0)
    {
        cmd = new_command(0);
        if (parse_command(recv_buff, cmd) == -1 || cmd->cid != LOGIN)
        {
            fprintf(stderr, "Error -- in LOGIN message\n");
            close(sockfd);
            free(cmd);
            free(recv_buff);
            return NULL;
        }

        cmd->sock = sockfd;
        if (process_command(cmd, &answer) == -1)
        {
            fprintf(stderr, "Error -- in LOGIN\n");
            close(sockfd);
            free(cmd);
            free(recv_buff);
            return NULL;
        }

        cl_key = cmd->key;
        if (send_answer_to_client(answer) == -1)
        {
            fprintf(stderr, "Error -- in LOGIN ack\n");
            close(sockfd);
            free(cmd);
            free_answer(answer);
            free(recv_buff);
            return NULL;
        }

        free_answer(answer);
        free(recv_buff);
    }

    while (network_recv(sockfd, (void **)&recv_buff) > 0)
    {
        cmd = new_command(cl_key);
        if (parse_command(recv_buff, cmd) == -1)
        {
            fprintf(stderr, "Warning: unable to parse message\n");
            notify_parse_error(cmd, recv_buff, &answer);
            send_answer_to_client(answer);
            free_answer(answer);
            free(cmd);
        }
        else
        {
            pthread_mutex_lock(&buff_mutex);
            while (is_buff_full())
            {
                pthread_cond_wait(&buff_not_full, &buff_mutex);
            }
            add_to_buff(cmd);
            pthread_cond_signal(&buff_not_empty);
            pthread_mutex_unlock(&buff_mutex);
        }
        free(recv_buff);
    }

    // Unregister client on disconnection
    cmd = new_command(cl_key);
    cmd->cid = UNREGISTER;
    if (process_command(cmd, &answer) == -1)
    {
        fprintf(stderr, "Warning -- failed to unregister client %s\n", client_name);
    }
    free(cmd);
    close(sockfd);
    return NULL;
}

void *executor_thread(void *arg)
{
    fastRandomSetSeed(time(NULL) + pthread_self() * 100);
    while (1)
    {
        pthread_mutex_lock(&buff_mutex);
        while (is_buffer_empty())
        {
            pthread_cond_wait(&buff_not_empty, &buff_mutex);
        }
        command_t *cmd = rmv_from_buff();
        pthread_cond_signal(&buff_not_full);
        pthread_mutex_unlock(&buff_mutex);

        answer_t *answer = NULL;
        int res = process_command(cmd, &answer);
        
        if (res == -1)
        {
            fprintf(stderr, "Warning: unable to process command\n");
        }
        if (answer && send_answer_to_client(answer) == -1)
        {
            fprintf(stderr, "Warning: unable to send answer to client\n");
        }
        free_answer(answer);
        free(cmd);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int sockfd, newsockfd;
    int portno = BABBLE_PORT;
    int opt;

    while ((opt = getopt(argc, argv, "+hp:r")) != -1)
    {
        switch (opt)
        {
        case 'p':
            portno = atoi(optarg);
            break;
        case 'r':
            random_delay_activated = 1;
            break;
        case 'h':
        default:
            display_help(argv[0]);
            return -1;
        }
    }

    // Initialize server data structures
    server_data_init();

    // start the exec threads
    for (int i = 0; i < BABBLE_EXECUTOR_THREADS; i++)
    {
        pthread_create(&exec_threads[i], NULL, executor_thread, NULL);
    }

    // Initialize and open the server socket
    if ((sockfd = server_connection_init(portno)) == -1)
    {
        return -1;
    }
    printf("Babble server bound to port %d\n", portno);

    // Main server loop
    while (1)
    {
        if ((newsockfd = server_connection_accept(sockfd)) == -1)
        {
            return -1;
        }
        int *client_sock = malloc(sizeof(int));
        *client_sock = newsockfd;
        pthread_t comm_tid;
        // start comm thread
        pthread_create(&comm_tid, NULL, communication_thread, client_sock);
        pthread_detach(comm_tid);
    }

    for (int i = 0; i < BABBLE_EXECUTOR_THREADS; i++)
    {
        pthread_join(exec_threads[i], NULL);
    }
    

    close(sockfd);
    return 0;
}