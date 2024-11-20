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

/* Command buffer for producer-consumer model */
static command_t *command_buffer[BABBLE_BUFFER_SIZE];
static int buffer_head = 0;
static int buffer_tail = 0;

/* Synchronization primitives for buffer */
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

pthread_t executor_threads[BABBLE_EXECUTOR_THREADS];

/* Flag for random delay activation */
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

/* Buffer Operations */
void add_command_to_buffer(command_t *cmd)
{
    command_buffer[buffer_tail] = cmd;
    buffer_tail = (buffer_tail + 1) % BABBLE_BUFFER_SIZE;
}

command_t *remove_command_from_buffer()
{
    command_t *cmd = command_buffer[buffer_head];
    buffer_head = (buffer_head + 1) % BABBLE_BUFFER_SIZE;
    return cmd;
}

int is_buffer_full()
{
    return ((buffer_tail + 1) % BABBLE_BUFFER_SIZE) == buffer_head;
}

int is_buffer_empty()
{
    return buffer_head == buffer_tail;
}

/* Communication thread function */
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
    // Handle login command
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

    // Process subsequent client commands
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
            pthread_mutex_lock(&buffer_mutex);
            while (is_buffer_full())
            {
                pthread_cond_wait(&buffer_not_full, &buffer_mutex);
            }
            add_command_to_buffer(cmd);
            pthread_cond_signal(&buffer_not_empty);
            pthread_mutex_unlock(&buffer_mutex);
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

/* Executor thread function */
void *executor_thread(void *arg)
{
    fastRandomSetSeed(time(NULL) + pthread_self() * 100);
    while (1)
    {
        pthread_mutex_lock(&buffer_mutex);
        while (is_buffer_empty())
        {
            pthread_cond_wait(&buffer_not_empty, &buffer_mutex);
        }
        command_t *cmd = remove_command_from_buffer();
        pthread_cond_signal(&buffer_not_full);
        pthread_mutex_unlock(&buffer_mutex);

        answer_t *answer = NULL;
        if (process_command(cmd, &answer) == -1)
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

    // Start the executor threadS
    for (int i = 0; i < BABBLE_EXECUTOR_THREADS; i++)
    {
        pthread_create(&executor_threads[i], NULL, executor_thread, NULL);
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
        pthread_create(&comm_tid, NULL, communication_thread, client_sock);
        pthread_detach(comm_tid);
    }

    for (int i = 0; i < BABBLE_EXECUTOR_THREADS; i++)
    {
        pthread_join(executor_threads[i], NULL);
    }
    

    close(sockfd);
    return 0;
}
