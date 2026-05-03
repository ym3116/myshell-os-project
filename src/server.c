/* =============================================================================
 * src/server.c  –  Phase 4 Remote Shell Server (Scheduler-Backed)
 *
 * Upgrades Phase 3 with a CPU scheduler so that multiple client requests are
 * served concurrently under a single-CPU model:
 *
 *   - Main thread  : accepts TCP connections and spawns one client thread each.
 *   - Client threads: receive commands from a connected client and enqueue them
 *                     as Task objects; they do NOT execute commands themselves.
 *   - Scheduler thread (in scheduler.c): dequeues tasks, applies SJRF + RR,
 *                     runs programs with SIGSTOP/SIGCONT preemption, and
 *                     streams output back to each client's socket.
 *
 * Protocol (unchanged from Phase 3):
 *   Client → Server : null-terminated command string
 *   Server → Client : [uint32_t length (network byte order)] [output bytes]
 *                     length == 0 signals end-of-stream for program tasks.
 *
 * Usage: ./myshell_server <port>
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "parser.h"
#include "exec.h"
#include "scheduler.h"

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */
#define BACKLOG  10     /* maximum pending connections before accept() blocks */
#define MAX_CMD  4096   /* maximum command length from a client               */

/* ---------------------------------------------------------------------------
 * Globals
 *
 * counter_mutex   : protects client_counter for unique ID assignment.
 * client_counter  : monotonically increasing client identifier.
 * --------------------------------------------------------------------------- */
static pthread_mutex_t counter_mutex  = PTHREAD_MUTEX_INITIALIZER;
static int             client_counter = 0;

/* ---------------------------------------------------------------------------
 * ClientArgs – passed (heap-allocated) to each client thread
 * --------------------------------------------------------------------------- */
typedef struct {
    int  client_fd;
    char client_ip[INET_ADDRSTRLEN];
    int  client_port;
    int  client_num;
} ClientArgs;


/* ---------------------------------------------------------------------------
 * handle_client() – per-client session loop
 *
 * Receives commands over the socket, classifies each as a shell command or
 * program task, creates a Task, and hands it to the scheduler via
 * scheduler_add_task().  For shell commands the scheduler runs them
 * synchronously and replies immediately; for program tasks the scheduler
 * streams output asynchronously.
 *
 * The client thread blocks on recv() waiting for the next command; it does
 * not wait for the scheduler to finish the previous one, which is correct
 * because program output is streamed directly from the scheduler thread.
 * --------------------------------------------------------------------------- */
static void handle_client(ClientArgs *info)
{
    int client_fd  = info->client_fd;
    int client_num = info->client_num;

    char cmd_buf[MAX_CMD];

    /* Log the new connection */
    pthread_mutex_lock(&g_print_mutex);
    printf("[%d]<<< client connected\n", client_num);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mutex);

    while (1) {
        memset(cmd_buf, 0, sizeof(cmd_buf));
        ssize_t n = recv(client_fd, cmd_buf, sizeof(cmd_buf) - 1, 0);

        if (n <= 0) {
            /* Client disconnected or network error */
            if (n < 0) perror("[ERROR] recv");
            break;
        }
        cmd_buf[n] = '\0';

        /* Strip trailing newline/carriage-return if present */
        size_t len = strlen(cmd_buf);
        while (len > 0 && (cmd_buf[len-1] == '\n' || cmd_buf[len-1] == '\r'))
            cmd_buf[--len] = '\0';

        pthread_mutex_lock(&g_print_mutex);
        printf("[%d]>>> %s\n", client_num, cmd_buf);
        fflush(stdout);
        pthread_mutex_unlock(&g_print_mutex);

        /* Handle built-in exit */
        if (strcmp(cmd_buf, "exit") == 0) {
            uint32_t zero = htonl(0);
            send_all_fd(client_fd, &zero, sizeof(zero));
            pthread_mutex_lock(&g_print_mutex);
            printf("[%d]--- client requested disconnect\n", client_num);
            fflush(stdout);
            pthread_mutex_unlock(&g_print_mutex);
            break;
        }

        /* Create a Task and hand it to the scheduler */
        Task *t = task_create(cmd_buf, client_fd, client_num);
        if (!t) {
            /* Memory allocation failure: send empty reply so client doesn't hang */
            uint32_t zero = htonl(0);
            send_all_fd(client_fd, &zero, sizeof(zero));
            continue;
        }

        scheduler_add_task(t);

        /*
         * For shell commands the scheduler sends the reply and the client can
         * immediately send another command.
         *
         * For program tasks the scheduler streams output over time; the client
         * reads until it sees a zero-length packet (end-of-stream marker).
         *
         * The client thread does not block here — it loops back to recv() to
         * accept the next command while the scheduler runs the current task.
         */
    }

    /*
     * Client disconnected: remove any pending tasks it submitted so the
     * scheduler does not try to send output to a closed socket.
     */
    scheduler_remove_client_tasks(client_num);

    close(client_fd);

    pthread_mutex_lock(&g_print_mutex);
    printf("[%d]--- client disconnected\n", client_num);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mutex);
}


/* ---------------------------------------------------------------------------
 * client_thread() – POSIX thread entry point
 * --------------------------------------------------------------------------- */
static void *client_thread(void *arg)
{
    ClientArgs *info = (ClientArgs *)arg;
    handle_client(info);
    free(info);
    return NULL;
}


/* ---------------------------------------------------------------------------
 * main() – server entry point
 *
 * 1. Initialise and start the scheduler.
 * 2. Create and bind the TCP listening socket.
 * 3. Accept client connections in a loop, spawning a detached thread each time.
 * --------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[ERROR] Invalid port: %s\n", argv[1]);
        return 1;
    }

    /* Initialise scheduler data structures and spawn scheduler thread */
    scheduler_init();
    if (scheduler_start() != 0) {
        fprintf(stderr, "[ERROR] Failed to start scheduler thread\n");
        return 1;
    }

    /* Create TCP socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[ERROR] socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[ERROR] bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("[ERROR] listen"); close(server_fd); return 1;
    }

    printf("| Hello, Server Started |\n");
    fflush(stdout);

    /* Accept loop */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) { perror("[ERROR] accept"); continue; }

        /* Assign unique client number */
        pthread_mutex_lock(&counter_mutex);
        int client_num = ++client_counter;
        pthread_mutex_unlock(&counter_mutex);

        ClientArgs *args = malloc(sizeof(ClientArgs));
        if (!args) {
            perror("[ERROR] malloc");
            close(client_fd);
            continue;
        }

        args->client_fd   = client_fd;
        args->client_num  = client_num;
        args->client_port = ntohs(client_addr.sin_port);
        strncpy(args->client_ip, inet_ntoa(client_addr.sin_addr),
                INET_ADDRSTRLEN - 1);
        args->client_ip[INET_ADDRSTRLEN - 1] = '\0';

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, args) != 0) {
            perror("[ERROR] pthread_create");
            close(client_fd);
            free(args);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
