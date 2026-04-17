/* =============================================================================
 * src/server.c  –  Phase 3 Remote Shell Server (Multithreaded)
 *
 * This server listens on a TCP port and executes shell commands received from
 * multiple simultaneous clients. Each accepted connection is handed off to a
 * dedicated POSIX thread so that clients are served concurrently.
 *
 * It uses the Phase 1 shell engine (parser + executor) to run commands and
 * sends the output back over the socket.
 *
 * Protocol:
 *   Client sends: null-terminated command string
 *   Server sends: [4-byte length][output bytes]
 *
 * Usage: ./myshell_server <port>
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* POSIX socket headers */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>   /* sockaddr_in, htonl, htons */
#include <arpa/inet.h>    /* inet_ntoa – for printing client IP */

/* POSIX threads */
#include <pthread.h>

#include "parser.h"
#include "exec.h"

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */
#define BACKLOG 10             /* max queued connections before accept() */
#define READ_CHUNK 4096        /* buffer size for reading command output */
#define MAX_OUTPUT (1024*1024) /* max output per command (1 MB) */
#define MAX_CMD 4096           /* max command length from client */

/* ---------------------------------------------------------------------------
 * Globals
 *
 * counter_mutex: protects client_counter so that each new connection gets a
 *                unique, monotonically-increasing client number even when
 *                multiple threads call accept() simultaneously.
 *
 * print_mutex:   serialises all printf/fflush calls across threads so that
 *                log lines from different clients do not interleave on stdout.
 *
 * client_counter: running total of accepted connections; used to assign each
 *                 client a human-readable "#N" identifier.
 * --------------------------------------------------------------------------- */
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t print_mutex   = PTHREAD_MUTEX_INITIALIZER;
static int             client_counter = 0;

/* ---------------------------------------------------------------------------
 * ClientArgs
 *
 * Heap-allocated struct passed to each client thread.  All fields are filled
 * in by main() before pthread_create() so the thread has everything it needs
 * without touching shared state.
 *
 * Fields:
 *   client_fd   – connected socket file descriptor
 *   client_ip   – dotted-decimal IP string of the remote peer
 *   client_port – remote port number (host byte order)
 *   client_num  – unique sequential client identifier assigned by main()
 * --------------------------------------------------------------------------- */
typedef struct {
    int  client_fd;
    char client_ip[INET_ADDRSTRLEN];
    int  client_port;
    int  client_num;
} ClientArgs;


/* ---------------------------------------------------------------------------
 * capture_command_output()
 *
 * Executes a parsed pipeline and captures all stdout/stderr output into a
 * heap-allocated buffer. Uses fork+pipe to redirect child output.
 *
 * Returns: heap-allocated output buffer (caller must free), sets *out_len
 * --------------------------------------------------------------------------- */
static char *capture_command_output(const Pipeline *pl, size_t *out_len)
{
    *out_len = 0;

    /* Create pipe for capturing child output */
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        perror("[ERROR] pipe creation failed");
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[ERROR] fork failed");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    if (pid == 0) {
        /* Child: redirect stdout/stderr to pipe, then execute */
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        execute_pipeline(pl);
        exit(0);
    }

    /* Parent: close write end and read all output from child */
    close(pipe_fds[1]);

    size_t capacity = READ_CHUNK;
    size_t total    = 0;
    char  *buf      = malloc(capacity);

    if (!buf) {
        /* Memory allocation failed - drain pipe and return empty */
        char discard[256];
        while (read(pipe_fds[0], discard, sizeof(discard)) > 0) {}
        close(pipe_fds[0]);
        waitpid(pid, NULL, 0);
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    /* Read output in chunks, growing buffer as needed */
    ssize_t n;
    while ((n = read(pipe_fds[0], buf + total, capacity - total)) > 0) {
        total += (size_t)n;
        if (total == capacity) {
            if (capacity >= MAX_OUTPUT) break;
            size_t new_cap = capacity * 2;
            if (new_cap > MAX_OUTPUT) new_cap = MAX_OUTPUT;
            char *tmp = realloc(buf, new_cap);
            if (!tmp) break;
            buf      = tmp;
            capacity = new_cap;
        }
    }

    close(pipe_fds[0]);
    waitpid(pid, NULL, 0);

    buf[total] = '\0';
    *out_len   = total;
    return buf;
}


/* ---------------------------------------------------------------------------
 * send_all() - Sends exactly len bytes, handling partial sends
 * Returns 0 on success, -1 on error
 * --------------------------------------------------------------------------- */
static int send_all(int fd, const void *buf, size_t len)
{
    const char *ptr = (const char *)buf;
    while (len > 0) {
        ssize_t sent = send(fd, ptr, len, 0);
        if (sent <= 0) return -1;
        ptr += sent;
        len -= (size_t)sent;
    }
    return 0;
}


/* ---------------------------------------------------------------------------
 * handle_client() - Main loop for serving one connected client
 *
 * Receives commands over the socket, executes them through the Phase 1 shell
 * engine, and sends output back using the 4-byte length-prefix protocol.
 * Runs entirely inside a dedicated thread; closes client_fd before returning.
 *
 * All printf/fflush pairs are wrapped with print_mutex so that log lines from
 * concurrent client threads do not interleave on stdout.
 * --------------------------------------------------------------------------- */
static void handle_client(ClientArgs *info)
{
    /* Extract connection details from the argument struct for convenience */
    int         client_fd   = info->client_fd;
    const char *client_ip   = info->client_ip;
    int         client_port = info->client_port;
    int         client_num  = info->client_num;

    char cmd_buf[MAX_CMD];

    /* Log the new connection with IP, port, and assigned thread identifier */
    pthread_mutex_lock(&print_mutex);
    printf("[INFO] Client #%d connected from %s:%d. Assigned to Thread-%d.\n",
           client_num, client_ip, client_port, client_num);
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);

    while (1) {
        /* Receive command from client */
        memset(cmd_buf, 0, sizeof(cmd_buf));
        ssize_t n = recv(client_fd, cmd_buf, sizeof(cmd_buf) - 1, 0);

        /* Check for disconnect or error */
        if (n <= 0) {
            if (n < 0) {
                perror("[ERROR] recv failed");
            }
            /* Client closed the connection without sending "exit" */
            break;
        }

        cmd_buf[n] = '\0';

        /* Log the received command with client identity */
        pthread_mutex_lock(&print_mutex);
        printf("[RECEIVED] [Client #%d - %s:%d] Received command: \"%s\"\n",
               client_num, client_ip, client_port, cmd_buf);
        fflush(stdout);
        pthread_mutex_unlock(&print_mutex);

        /* Handle exit command */
        if (strcmp(cmd_buf, "exit") == 0) {
            uint32_t zero = 0;
            send_all(client_fd, &zero, sizeof(zero));
            pthread_mutex_lock(&print_mutex);
            printf("[INFO] [Client #%d - %s:%d] Client requested disconnect. Closing connection.\n",
                   client_num, client_ip, client_port);
            fflush(stdout);
            pthread_mutex_unlock(&print_mutex);
            break;
        }

        /* Log which client's command is now being executed */
        pthread_mutex_lock(&print_mutex);
        printf("[EXECUTING] [Client #%d - %s:%d] Executing command: \"%s\"\n",
               client_num, client_ip, client_port, cmd_buf);
        fflush(stdout);
        pthread_mutex_unlock(&print_mutex);

        /* Parse the command */
        Pipeline pl;
        char errbuf[256];
        int rc = parse_line(cmd_buf, &pl, errbuf, sizeof(errbuf));

        if (rc != 0) {
            /* Parse error - send error message to client */
            const char *msg = (errbuf[0] != '\0') ? errbuf : "Parse error.\n";
            size_t msg_len = strlen(msg);
            uint32_t net_len = htonl((uint32_t)msg_len);
            send_all(client_fd, &net_len, sizeof(net_len));
            send_all(client_fd, msg, msg_len);
            pthread_mutex_lock(&print_mutex);
            printf("[ERROR] [Client #%d - %s:%d] Parse error: %s\n",
                   client_num, client_ip, client_port, msg);
            printf("[OUTPUT] [Client #%d - %s:%d] Sending error message to client: \"%s\"\n",
                   client_num, client_ip, client_port, msg);
            fflush(stdout);
            pthread_mutex_unlock(&print_mutex);
            continue;
        }

        /* Execute and capture output */
        size_t out_len = 0;
        char *output = capture_command_output(&pl, &out_len);
        free_pipeline(&pl);

        /* Check if output contains "Command not found" error */
        pthread_mutex_lock(&print_mutex);
        if (output && (strstr(output, "Command not found") != NULL)) {
            printf("[ERROR] [Client #%d - %s:%d] Command not found: \"%s\"\n",
                   client_num, client_ip, client_port, cmd_buf);
            printf("[OUTPUT] [Client #%d - %s:%d] Sending error message to client: \"%s\"\n",
                   client_num, client_ip, client_port, output);
        } else {
            printf("[OUTPUT] [Client #%d - %s:%d] Sending output to client:\n",
                   client_num, client_ip, client_port);
            if (out_len > 0) {
                printf("%s", output);
            } else {
                /* Command produced no output — may have redirected to a file or
                 * completed silently (e.g. cd, mkdir). Send zero-length response. */
                printf("(no output — may have been redirected to a file)\n");
            }
        }
        fflush(stdout);
        pthread_mutex_unlock(&print_mutex);

        /* Send output to client using length-prefix protocol */
        uint32_t net_len = htonl((uint32_t)out_len);
        send_all(client_fd, &net_len, sizeof(net_len));
        if (out_len > 0) {
            send_all(client_fd, output, out_len);
        }

        free(output);
    }

    /* The thread owns client_fd; close it here so main() never has to wait */
    close(client_fd);

    /* Log final disconnection with the client's identifier */
    pthread_mutex_lock(&print_mutex);
    printf("[INFO] Client #%d disconnected.\n", client_num);
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);
}


/* ---------------------------------------------------------------------------
 * client_thread() - POSIX thread entry point for each client connection
 *
 * Unpacks the ClientArgs pointer, calls handle_client(), then frees the
 * heap-allocated args struct.  Declared as returning void* to match the
 * pthread_create() signature; always returns NULL.
 * --------------------------------------------------------------------------- */
static void *client_thread(void *arg)
{
    ClientArgs *info = (ClientArgs *)arg;
    handle_client(info);  /* runs the full client session, closes client_fd */
    free(info);           /* release the heap struct allocated in main() */
    return NULL;
}


/* ---------------------------------------------------------------------------
 * main() - Server entry point
 *
 * Sets up TCP socket, binds to port, and accepts client connections in a
 * loop.  For each accepted connection a ClientArgs struct is heap-allocated,
 * filled in, and handed to a new detached thread via pthread_create().
 * main() never blocks on a client — it loops immediately back to accept().
 * --------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* Validate command line arguments */
    if (argc != 2) {
        fprintf(stderr, "[ERROR] Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[ERROR] Invalid port number: %s\n", argv[1]);
        return 1;
    }

    /* Create TCP socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[ERROR] socket creation failed");
        return 1;
    }

    /* Allow port reuse to avoid "Address already in use" on restart */
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[ERROR] setsockopt failed");
        close(server_fd);
        return 1;
    }

    /* Bind to all interfaces on specified port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[ERROR] bind failed");
        close(server_fd);
        return 1;
    }

    /* Start listening for connections */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("[ERROR] listen failed");
        close(server_fd);
        return 1;
    }

    printf("[INFO] Server started, waiting for client connections...\n");
    fflush(stdout);

    /* Main accept loop - spawn a thread per client so all are served
     * simultaneously without blocking on any individual connection */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) {
            perror("[ERROR] accept failed");
            continue;
        }

        /* Assign a unique client number under the counter lock so that two
         * threads racing through accept() cannot receive the same number */
        pthread_mutex_lock(&counter_mutex);
        int client_num = ++client_counter;
        pthread_mutex_unlock(&counter_mutex);

        /* Heap-allocate ClientArgs — must NOT be a local/stack variable
         * because main() returns to the top of the loop immediately after
         * pthread_create(); a stack variable would be clobbered */
        ClientArgs *args = malloc(sizeof(ClientArgs));
        if (!args) {
            /* If we can't allocate memory, reject this connection gracefully */
            perror("[ERROR] malloc failed for ClientArgs");
            close(client_fd);
            continue;
        }

        /* Fill in connection details before handing the struct to the thread */
        args->client_fd   = client_fd;
        args->client_num  = client_num;
        args->client_port = ntohs(client_addr.sin_port);
        strncpy(args->client_ip,
                inet_ntoa(client_addr.sin_addr),
                INET_ADDRSTRLEN - 1);
        args->client_ip[INET_ADDRSTRLEN - 1] = '\0';  /* ensure NUL-termination */

        /* Spawn a thread to handle this client */
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, args) != 0) {
            /* Thread creation failed; clean up and try the next connection */
            perror("[ERROR] pthread_create failed");
            close(client_fd);
            free(args);
            continue;
        }

        /* Detach the thread so its resources are reclaimed automatically
         * when it exits — main() never calls pthread_join() */
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
