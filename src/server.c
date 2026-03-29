/* =============================================================================
 * src/server.c  –  Phase 2 Remote Shell Server
 *
 * Responsibility:
 *   Listens on a TCP port for exactly one client connection.  For each command
 *   the client sends, the server:
 *     1. Prints the received command to its own terminal (server log).
 *     2. Executes the command using the Phase 1 shell engine
 *        (parse_line + execute_pipeline), capturing all stdout/stderr output
 *        via a pipe rather than letting it reach the terminal.
 *     3. Sends the captured output back to the client over the socket using a
 *        simple length-prefix framing protocol:
 *          [ 4 bytes: uint32_t output length in network byte order ]
 *          [ N bytes: raw output text ]
 *     4. Prints a confirmation log line.
 *
 * Usage:
 *   ./myshell_server <port>
 *
 * Example:
 *   ./myshell_server 9090
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

/* Maximum number of queued incoming connections before accept() is called */
#define BACKLOG 1

/* Size of the buffer used to read output from the command pipe in one chunk */
#define READ_CHUNK 4096

/* Maximum total output we will buffer per command (1 MB).
 * Commands that produce more output are truncated. */
#define MAX_OUTPUT (1024 * 1024)

/* Maximum length of a single command line received from the client */
#define MAX_CMD 4096


/* ---------------------------------------------------------------------------
 * capture_command_output()
 *
 * Runs one shell command (already parsed into a Pipeline) and captures
 * everything written to stdout and stderr into a heap-allocated buffer.
 *
 * Strategy:
 *   - Create a pipe: pipe_fds[0]=read end, pipe_fds[1]=write end.
 *   - Fork a child process.
 *   - In the child: dup2 both STDOUT and STDERR to the write end, then close
 *     the read end and the original write end, then execute the pipeline.
 *   - In the parent: close the write end (so we see EOF when the child exits),
 *     then read all output from the read end into a dynamically grown buffer.
 *   - waitpid() to reap the child.
 *
 * Parameters:
 *   pl       – pointer to the parsed pipeline (ready to execute)
 *   out_len  – [out] set to the number of bytes captured
 *
 * Returns:
 *   Heap-allocated buffer containing the output (caller must free()).
 *   Returns an empty heap string "" if there is no output or on error.
 * --------------------------------------------------------------------------- */
static char *capture_command_output(const Pipeline *pl, size_t *out_len)
{
    *out_len = 0;

    /* Create a pipe: the child writes output into pipe_fds[1],
     * the parent reads captured output from pipe_fds[0]. */
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        perror("[Server] pipe");
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[Server] fork");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    if (pid == 0) {
        /* ----------------------------------------------------------------
         * CHILD PROCESS
         * Redirect both stdout and stderr to the write end of the pipe so
         * that ALL output produced by execute_pipeline() is captured.
         * ---------------------------------------------------------------- */
        close(pipe_fds[0]);                          /* don't need read end */
        dup2(pipe_fds[1], STDOUT_FILENO);            /* stdout → pipe       */
        dup2(pipe_fds[1], STDERR_FILENO);            /* stderr → pipe       */
        close(pipe_fds[1]);                          /* fd now duplicated   */

        /* Execute the pipeline using the Phase 1 engine */
        execute_pipeline(pl);
        exit(0);
    }

    /* -----------------------------------------------------------------------
     * PARENT PROCESS
     * Close the write end — if we don't, read() below will never see EOF
     * because the parent itself holds an open write end.
     * ----------------------------------------------------------------------- */
    close(pipe_fds[1]);

    /* Read the child's output in chunks, growing the buffer as needed */
    size_t capacity = READ_CHUNK;
    size_t total    = 0;
    char  *buf      = malloc(capacity);
    if (!buf) {
        /* OOM: drain and discard the pipe, reap child, return empty */
        char discard[256];
        while (read(pipe_fds[0], discard, sizeof(discard)) > 0) {}
        close(pipe_fds[0]);
        waitpid(pid, NULL, 0);
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    ssize_t n;
    while ((n = read(pipe_fds[0], buf + total, capacity - total)) > 0) {
        total += (size_t)n;

        /* If the buffer is full, double its capacity (up to MAX_OUTPUT) */
        if (total == capacity) {
            if (capacity >= MAX_OUTPUT) break;          /* hard cap reached */
            size_t new_cap = capacity * 2;
            if (new_cap > MAX_OUTPUT) new_cap = MAX_OUTPUT;
            char *tmp = realloc(buf, new_cap);
            if (!tmp) break;                            /* OOM: keep what we have */
            buf      = tmp;
            capacity = new_cap;
        }
    }

    close(pipe_fds[0]);
    waitpid(pid, NULL, 0);  /* reap child to avoid zombies */

    buf[total] = '\0';
    *out_len   = total;
    return buf;
}


/* ---------------------------------------------------------------------------
 * send_all()
 *
 * Wrapper around send() that keeps sending until all `len` bytes have been
 * transmitted or an error occurs (send() may send fewer bytes than requested
 * on a single call for large buffers).
 *
 * Returns 0 on success, -1 on error.
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
 * handle_client()
 *
 * Main loop for serving one connected client.  Runs until the client sends
 * "exit", closes the connection, or a network error occurs.
 *
 * Protocol (per command):
 *   Client → Server : command string (null-terminated, up to MAX_CMD bytes)
 *   Server → Client : uint32_t length  (4 bytes, network byte order)
 *                   + output bytes     (length bytes of raw text)
 *
 * Parameters:
 *   client_fd  – the connected socket file descriptor for this client
 *   client_ip  – printable IP string used only for log messages
 * --------------------------------------------------------------------------- */
static void handle_client(int client_fd, const char *client_ip)
{
    char cmd_buf[MAX_CMD];

    printf("[Server] Client connected: %s\n", client_ip);
    fflush(stdout);

    while (1) {
        /* ----------------------------------------------------------------
         * Receive the next command from the client.
         * We use recv() with MSG_WAITALL to block until either the full
         * buffer is filled or the connection closes.  In practice the client
         * sends a null-terminated string so we just read up to MAX_CMD-1.
         * ---------------------------------------------------------------- */
        memset(cmd_buf, 0, sizeof(cmd_buf));
        ssize_t n = recv(client_fd, cmd_buf, sizeof(cmd_buf) - 1, 0);

        if (n <= 0) {
            /* n == 0  → client closed the connection gracefully
             * n <  0  → network error                              */
            if (n < 0) perror("[Server] recv");
            printf("[Server] Client disconnected: %s\n", client_ip);
            fflush(stdout);
            break;
        }

        cmd_buf[n] = '\0';   /* ensure null-termination */

        /* Print the received command to the server terminal */
        printf("[Server] Received command from %s: %s\n", client_ip, cmd_buf);
        fflush(stdout);

        /* ----------------------------------------------------------------
         * Handle the built-in "exit" command: acknowledge the client and
         * close the connection.
         * ---------------------------------------------------------------- */
        if (strcmp(cmd_buf, "exit") == 0) {
            /* Send an empty response (length = 0) so the client doesn't hang */
            uint32_t zero = 0;
            send_all(client_fd, &zero, sizeof(zero));
            printf("[Server] Client requested exit. Closing connection.\n");
            fflush(stdout);
            break;
        }

        /* ----------------------------------------------------------------
         * Parse the command using the Phase 1 parser.
         * ---------------------------------------------------------------- */
        Pipeline pl;
        char     errbuf[256];

        int rc = parse_line(cmd_buf, &pl, errbuf, sizeof(errbuf));
        if (rc != 0) {
            /* Parsing failed: send the error message back to the client */
            const char *msg = (errbuf[0] != '\0') ? errbuf : "Parse error.\n";
            size_t msg_len  = strlen(msg);
            uint32_t net_len = htonl((uint32_t)msg_len);
            send_all(client_fd, &net_len, sizeof(net_len));
            send_all(client_fd, msg,      msg_len);
            printf("[Server] Parse error for command: %s\n", cmd_buf);
            fflush(stdout);
            continue;
        }

        /* ----------------------------------------------------------------
         * Execute the pipeline and capture its output.
         * ---------------------------------------------------------------- */
        size_t out_len = 0;
        char  *output  = capture_command_output(&pl, &out_len);
        free_pipeline(&pl);

        /* ----------------------------------------------------------------
         * Send the output back to the client using the length-prefix protocol:
         *   [ 4-byte uint32_t: output length in network byte order ]
         *   [ out_len bytes  : the actual output text               ]
         * ---------------------------------------------------------------- */
        uint32_t net_len = htonl((uint32_t)out_len);
        send_all(client_fd, &net_len, sizeof(net_len));
        if (out_len > 0) {
            send_all(client_fd, output, out_len);
        }

        printf("[Server] Sent output (%zu bytes) to %s\n", out_len, client_ip);
        fflush(stdout);

        free(output);
    }
}


/* ---------------------------------------------------------------------------
 * main()
 *
 * Entry point for the server.
 *
 * 1. Validate the port argument.
 * 2. Create a TCP socket and set SO_REUSEADDR so the port can be reused
 *    immediately after restart (avoids "Address already in use" errors).
 * 3. Bind to 0.0.0.0:<port> so all network interfaces are accepted.
 * 4. Listen for incoming connections.
 * 5. Accept one client at a time and call handle_client().
 *    After handle_client() returns, loop back to accept the next client.
 * --------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* ------------------------------------------------------------------
     * Argument validation
     * ------------------------------------------------------------------ */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[Server] Invalid port number: %s\n", argv[1]);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Create a TCP socket (IPv4, stream-based)
     * AF_INET     = IPv4
     * SOCK_STREAM = TCP (reliable, connection-oriented)
     * 0           = OS chooses the appropriate protocol (TCP)
     * ------------------------------------------------------------------ */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[Server] socket");
        return 1;
    }

    /* Allow the port to be reused immediately after the server is restarted.
     * Without this, the OS keeps the port in TIME_WAIT state for ~60 seconds
     * after the server exits, causing "Address already in use" on restart. */
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[Server] setsockopt");
        close(server_fd);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Bind the socket to all network interfaces (INADDR_ANY) on our port
     * ------------------------------------------------------------------ */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* accept on all interfaces */
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[Server] bind");
        close(server_fd);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Start listening — BACKLOG limits queued connections waiting for accept()
     * ------------------------------------------------------------------ */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("[Server] listen");
        close(server_fd);
        return 1;
    }

    printf("[Server] Listening on port %d...\n", port);
    fflush(stdout);

    /* ------------------------------------------------------------------
     * Main accept loop: handle one client connection at a time.
     * After a client disconnects, wait for the next one.
     * ------------------------------------------------------------------ */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* accept() blocks until a client connects, then returns a new socket
         * file descriptor dedicated to that client connection */
        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) {
            perror("[Server] accept");
            continue;   /* non-fatal: try to accept the next client */
        }

        /* Convert the client's IP address to a printable string for logging */
        char *client_ip = inet_ntoa(client_addr.sin_addr);

        /* Serve this client until it disconnects or sends "exit" */
        handle_client(client_fd, client_ip);

        close(client_fd);
        printf("[Server] Connection closed. Waiting for next client...\n");
        fflush(stdout);
    }

    /* Unreachable under normal operation */
    close(server_fd);
    return 0;
}
