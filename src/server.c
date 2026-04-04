/* =============================================================================
 * src/server.c  –  Phase 2 Remote Shell Server
 *
 * This server listens on a TCP port and executes shell commands received from
 * a connected client. It uses the Phase 1 shell engine (parser + executor) to
 * run commands and sends the output back over the socket.
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

#include "parser.h"
#include "exec.h"

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */
#define BACKLOG 1              /* max queued connections before accept() */
#define READ_CHUNK 4096        /* buffer size for reading command output */
#define MAX_OUTPUT (1024*1024) /* max output per command (1 MB) */
#define MAX_CMD 4096           /* max command length from client */


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
 * Receives commands, executes them, and sends output back until client
 * disconnects or sends "exit".
 * --------------------------------------------------------------------------- */
static void handle_client(int client_fd, const char *client_ip)
{
    (void)client_ip;  /* suppress unused warning - we use generic messages */
    char cmd_buf[MAX_CMD];

    printf("[INFO] Client connected.\n");
    fflush(stdout);

    while (1) {
        /* Receive command from client */
        memset(cmd_buf, 0, sizeof(cmd_buf));
        ssize_t n = recv(client_fd, cmd_buf, sizeof(cmd_buf) - 1, 0);

        /* Check for disconnect or error */
        if (n <= 0) {
            if (n < 0) {
                perror("[ERROR] recv failed");
            }
            printf("[INFO] Client disconnected.\n");
            fflush(stdout);
            break;
        }

        cmd_buf[n] = '\0';

        /* Log received command */
        printf("[RECEIVED] Received command: \"%s\" from client.\n", cmd_buf);
        fflush(stdout);

        /* Handle exit command */
        if (strcmp(cmd_buf, "exit") == 0) {
            uint32_t zero = 0;
            send_all(client_fd, &zero, sizeof(zero));
            printf("[INFO] Client requested exit.\n");
            fflush(stdout);
            break;
        }

        /* Log execution */
        printf("[EXECUTING] Executing command: \"%s\"\n", cmd_buf);
        fflush(stdout);

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
            printf("[ERROR] Parse error: %s\n", msg);
            printf("[OUTPUT] Sending error message to client: \"%s\"\n", msg);
            fflush(stdout);
            continue;
        }

        /* Execute and capture output */
        size_t out_len = 0;
        char *output = capture_command_output(&pl, &out_len);
        free_pipeline(&pl);

        /* Check if output contains "Command not found" error */
        if (output && (strstr(output, "Command not found") != NULL)) {
            printf("[ERROR] Command not found: \"%s\"\n", cmd_buf);
            printf("[OUTPUT] Sending error message to client: \"%s\"\n", output);
        } else {
            printf("[OUTPUT] Sending output to client:\n");
            if (out_len > 0) {
                printf("%s", output);
            } else {
                /* Output is empty — command may have redirected output to a file,
                 * produced no output, or failed silently. Log this so the server
                 * terminal makes it clear something was sent (zero bytes). */
                printf("(no output — may have been redirected to a file)\n");
            }
        }
        fflush(stdout);

        /* Send output to client using length-prefix protocol */
        uint32_t net_len = htonl((uint32_t)out_len);
        send_all(client_fd, &net_len, sizeof(net_len));
        if (out_len > 0) {
            send_all(client_fd, output, out_len);
        }

        free(output);
    }
}


/* ---------------------------------------------------------------------------
 * main() - Server entry point
 *
 * Sets up TCP socket, binds to port, and accepts client connections.
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

    /* Main accept loop - handle one client at a time */
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

        char *client_ip = inet_ntoa(client_addr.sin_addr);
        handle_client(client_fd, client_ip);

        close(client_fd);
        printf("[INFO] Connection closed. Waiting for next client...\n");
        fflush(stdout);
    }

    close(server_fd);
    return 0;
}
