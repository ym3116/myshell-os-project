/* =============================================================================
 * src/client.c  –  Phase 2 Remote Shell Client
 *
 * Responsibility:
 *   Connects to a running myshell_server over TCP and provides an interactive
 *   shell-like interface to the user.  Every command the user types is sent to
 *   the server for execution; the server's output is received and printed here.
 *
 * Protocol (same as server side):
 *   Client → Server : command string (null-terminated, up to MAX_CMD bytes)
 *   Server → Client : uint32_t length  (4 bytes, network byte order)
 *                   + output bytes     (length bytes of raw text)
 *
 * Usage:
 *   ./myshell_client <server_host> <port>
 *
 * Example:
 *   ./myshell_client 127.0.0.1 9090
 *   ./myshell_client myserver.nyuad.nyu.edu 9090
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
#include <netinet/in.h>   /* sockaddr_in, htons */
#include <arpa/inet.h>    /* inet_pton – convert IP string → binary */
#include <netdb.h>        /* getaddrinfo – resolve hostnames */

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */

/* Maximum command length the client will send in one message */
#define MAX_CMD 4096

/* Size of the receive buffer used to read output chunks from the server */
#define RECV_BUF 4096


/* ---------------------------------------------------------------------------
 * recv_all()
 *
 * Guarantees that exactly `len` bytes are received from `fd` into `buf`.
 * The standard recv() may return fewer bytes than requested (e.g., when the
 * data arrives in multiple TCP segments), so we loop until all bytes arrive
 * or the connection is closed/broken.
 *
 * Returns:
 *    0  – all `len` bytes received successfully
 *   -1  – connection closed or error before all bytes arrived
 * --------------------------------------------------------------------------- */
static int recv_all(int fd, void *buf, size_t len)
{
    char   *ptr       = (char *)buf;
    size_t  remaining = len;

    while (remaining > 0) {
        ssize_t n = recv(fd, ptr, remaining, 0);
        if (n <= 0) {
            /* n == 0 → server closed connection
             * n <  0 → network error              */
            return -1;
        }
        ptr       += n;
        remaining -= (size_t)n;
    }
    return 0;
}


/* ---------------------------------------------------------------------------
 * connect_to_server()
 *
 * Resolves `host` (which may be a hostname or a dotted-decimal IP string)
 * and establishes a TCP connection to `host:port`.
 *
 * Using getaddrinfo() rather than inet_pton() directly means the client
 * accepts both IP addresses ("192.168.1.1") and hostnames ("myserver.edu").
 *
 * Returns:
 *   A connected socket file descriptor on success.
 *   Exits the process on failure (since there is nothing useful to do
 *   if we cannot reach the server).
 * --------------------------------------------------------------------------- */
static int connect_to_server(const char *host, int port)
{
    /* Convert the numeric port to a string for getaddrinfo() */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    /* Hints tell getaddrinfo() we want TCP (SOCK_STREAM) over IPv4 (AF_INET) */
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;    /* TCP  */

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "[Client] Cannot resolve host '%s': %s\n",
                host, gai_strerror(err));
        exit(1);
    }

    int sock = -1;

    /* getaddrinfo() may return multiple addresses; try each one */
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;    /* failed to create socket, try next */

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;   /* connected successfully */
        }

        /* connect() failed for this address — close and try the next one */
        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);

    if (sock < 0) {
        fprintf(stderr, "[Client] Could not connect to %s:%d – %s\n",
                host, port, strerror(errno));
        exit(1);
    }

    return sock;
}


/* ---------------------------------------------------------------------------
 * main()
 *
 * Entry point for the client.
 *
 * 1. Validate arguments and connect to the server.
 * 2. Enter the interactive loop:
 *      a. Print the "$ " prompt.
 *      b. Read a line of input from the user (via getline).
 *      c. Strip the trailing newline.
 *      d. Skip blank lines (just re-prompt).
 *      e. Send the command string to the server.
 *      f. Receive the 4-byte length header.
 *      g. Receive `length` bytes of output and print them.
 *      h. If the command was "exit", break out of the loop.
 * 3. Close the socket and exit cleanly.
 * --------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* ------------------------------------------------------------------
     * Argument validation
     * ------------------------------------------------------------------ */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_host> <port>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int         port = atoi(argv[2]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[Client] Invalid port number: %s\n", argv[2]);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Establish TCP connection to the server
     * ------------------------------------------------------------------ */
    int sock = connect_to_server(host, port);
    printf("[Client] Connected to server %s:%d\n", host, port);
    fflush(stdout);

    /* ------------------------------------------------------------------
     * Interactive command loop
     * ------------------------------------------------------------------ */
    char   *line = NULL;   /* getline manages this buffer automatically */
    size_t  cap  = 0;
    int     done = 0;      /* set to 1 when "exit" is processed         */

    while (!done) {
        /* Print the shell prompt */
        printf("$ ");
        fflush(stdout);

        /* Read a line from the user.
         * getline() returns -1 on EOF (Ctrl-D) or error. */
        ssize_t nread = getline(&line, &cap, stdin);
        if (nread < 0) {
            /* EOF: treat the same as "exit" */
            printf("\n");
            /* Send "exit" so the server also cleans up */
            send(sock, "exit", 4, 0);
            break;
        }

        /* Strip the trailing newline that getline() includes */
        if (nread > 0 && line[nread - 1] == '\n') {
            line[nread - 1] = '\0';
            nread--;
        }

        /* Skip blank/whitespace-only lines — just re-prompt */
        int blank = 1;
        for (ssize_t i = 0; i < nread; i++) {
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
                blank = 0;
                break;
            }
        }
        if (blank) continue;

        /* ----------------------------------------------------------------
         * Send the command to the server.
         * We send exactly strlen(line) bytes (the null terminator is not
         * needed because the server uses the recv() return value for length,
         * but we add +1 to transmit the '\0' so the server can use strcmp).
         * ---------------------------------------------------------------- */
        size_t cmd_len = (size_t)nread + 1;   /* include null terminator */
        if (send(sock, line, cmd_len, 0) < 0) {
            perror("[Client] send");
            break;
        }

        /* If the user typed "exit", note it — we still wait for the server's
         * zero-length acknowledgement before closing. */
        if (strcmp(line, "exit") == 0) {
            done = 1;
        }

        /* ----------------------------------------------------------------
         * Receive the 4-byte length header from the server.
         * The server always sends this, even for empty output (len = 0).
         * ---------------------------------------------------------------- */
        uint32_t net_len = 0;
        if (recv_all(sock, &net_len, sizeof(net_len)) < 0) {
            fprintf(stderr, "[Client] Server disconnected unexpectedly.\n");
            break;
        }

        /* Convert from network byte order to host byte order */
        uint32_t out_len = ntohl(net_len);

        /* ----------------------------------------------------------------
         * Receive and print the output in chunks.
         * We do NOT allocate one giant buffer; instead we receive RECV_BUF
         * bytes at a time and print each chunk immediately.  This keeps
         * memory usage bounded even for large command outputs.
         * ---------------------------------------------------------------- */
        uint32_t remaining = out_len;
        char     recv_buf[RECV_BUF + 1];

        while (remaining > 0) {
            uint32_t to_read = (remaining < RECV_BUF) ? remaining : RECV_BUF;
            ssize_t  n       = recv(sock, recv_buf, to_read, 0);
            if (n <= 0) {
                fprintf(stderr, "[Client] Connection lost while receiving output.\n");
                done = 1;
                break;
            }
            recv_buf[n] = '\0';
            printf("%s", recv_buf);    /* print chunk immediately */
            remaining -= (uint32_t)n;
        }

        /* Ensure the last chunk's output is flushed to the terminal */
        fflush(stdout);
    }

    /* ------------------------------------------------------------------
     * Cleanup
     * ------------------------------------------------------------------ */
    free(line);
    close(sock);
    printf("[Client] Disconnected from server.\n");
    return 0;
}
