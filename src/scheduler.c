/* =============================================================================
 * src/scheduler.c  –  Phase 4 Scheduler Implementation
 *
 * Implements the combined SJRF + Round-Robin scheduler.
 *
 * Thread model:
 *   - Client threads call scheduler_add_task() to enqueue work, then return
 *     immediately.  They do NOT wait for execution to finish here.
 *   - One scheduler thread (started by scheduler_start()) picks tasks from
 *     the queue, runs them, and streams output back to each client's socket.
 *
 * Scheduling rules (from the spec):
 *   1. Shell commands (burst_time == BURST_SHELL) run immediately; they are
 *      never queued beside program tasks and are never preempted.
 *   2. Program tasks are queued and selected by shortest remaining_time (SJRF).
 *   3. Quantum: QUANTUM_FIRST seconds on round 1, QUANTUM_REST on all later rounds.
 *   4. A running program task is preempted (SIGSTOP) if a new task with strictly
 *      shorter remaining_time arrives while it is running.
 *   5. The same task cannot be selected twice in a row if another task exists.
 *   6. Equal remaining_time → FCFS (arrival_time order).
 *   7. A "round" completes when the task is preempted (even if quantum not done).
 *   8. When a client disconnects, all its queued tasks are removed and any
 *      running task it owns is killed.
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

#include <stdarg.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "scheduler.h"
#include "parser.h"
#include "exec.h"

/* ---------------------------------------------------------------------------
 * Module-level globals
 * --------------------------------------------------------------------------- */

/* Shared print mutex – also declared extern in scheduler.h so server.c can use it */
pthread_mutex_t g_print_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ANSI color codes for server log output */
#define CLR_RESET   "\033[0m"
#define CLR_BLUE    "\033[34m"   /* created  */
#define CLR_GREEN   "\033[32m"   /* started  */
#define CLR_RED     "\033[31m"   /* ended    */
#define CLR_CYAN    "\033[36m"   /* waiting  */
#define CLR_YELLOW  "\033[33m"   /* running  */

/* Queue head (singly linked list, protected by g_queue_mutex) */
static Task           *g_queue_head   = NULL;

/* Protects g_queue_head and g_running_task */
static pthread_mutex_t g_queue_mutex  = PTHREAD_MUTEX_INITIALIZER;

/* Signals the scheduler thread when a new task is enqueued */
static pthread_cond_t  g_queue_cond   = PTHREAD_COND_INITIALIZER;

/* Monotonically increasing task ID counter */
static int             g_next_task_id = 1;

/* The task currently being executed by the scheduler (NULL if idle).
 * Written only by the scheduler thread; read by client threads to decide
 * whether to preempt. */
static Task           *g_running_task = NULL;

/* The task_id of the last completed/preempted task – enforces the
 * anti-consecutive rule: the same task cannot run twice in a row. */
static int             g_last_run_id  = -1;

/* Execution-timeline string (e.g. "0)-P5-(3)-P6-(1)-...") printed at end */
#define TIMELINE_MAX 4096
static char  g_timeline[TIMELINE_MAX] = "0)";
static int   g_timeline_time = 0;   /* cumulative wall-clock seconds */

/* Forward declaration so forward_output_for() can call check_preemption() */
static int check_preemption(Task *running);

/* ---------------------------------------------------------------------------
 * send_all_fd() – write exactly len bytes, retrying on short sends
 * --------------------------------------------------------------------------- */
int send_all_fd(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * log_print() – thread-safe single-line log helper
 * --------------------------------------------------------------------------- */
static void log_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_print_mutex);
    vprintf(fmt, ap);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mutex);
    va_end(ap);
}

/* ---------------------------------------------------------------------------
 * timeline_append() – append one scheduler event to the summary string
 * --------------------------------------------------------------------------- */
static void timeline_append(int client_num, int elapsed)
{
    g_timeline_time += elapsed;
    char entry[64];
    snprintf(entry, sizeof(entry), "-P%d-(%d)", client_num, g_timeline_time);

    /* Append safely; silently truncate if buffer is full */
    size_t remaining = TIMELINE_MAX - strlen(g_timeline) - 1;
    strncat(g_timeline, entry, remaining);
}

/* ---------------------------------------------------------------------------
 * is_demo_command() – returns 1 if cmd looks like "demo N" or "./demo N"
 *
 * Recognises:
 *   demo N
 *   ./demo N
 *   /path/to/demo N
 * Sets *n_out to the parsed N value on success.
 * --------------------------------------------------------------------------- */
static int is_demo_command(const char *cmd, int *n_out)
{
    if (!cmd || !n_out) return 0;

    const char *p = cmd;

    /* Skip leading whitespace */
    while (isspace((unsigned char)*p)) p++;

    /* Accept optional path prefix ending in '/' */
    const char *slash = strrchr(p, '/');
    if (slash) p = slash + 1;

    /* Must start with "demo" */
    if (strncmp(p, "demo", 4) != 0) return 0;
    p += 4;

    /* After "demo" must be whitespace then a positive integer */
    if (!isspace((unsigned char)*p)) return 0;
    while (isspace((unsigned char)*p)) p++;

    if (!isdigit((unsigned char)*p)) return 0;

    int n = atoi(p);
    if (n <= 0) return 0;

    *n_out = n;
    return 1;
}

/* ---------------------------------------------------------------------------
 * task_create() – allocate and initialise a Task from a raw command string
 * --------------------------------------------------------------------------- */
Task *task_create(const char *cmd, int client_fd, int client_num)
{
    Task *t = calloc(1, sizeof(Task));
    if (!t) return NULL;

    strncpy(t->cmd, cmd, MAX_CMD - 1);
    t->cmd[MAX_CMD - 1] = '\0';
    t->client_fd   = client_fd;
    t->client_num  = client_num;
    t->child_pid   = 0;
    t->out_pipe[0] = -1;
    t->out_pipe[1] = -1;
    t->state       = STATE_WAITING;
    t->arrival_time = time(NULL);
    t->round       = 0;
    t->next        = NULL;

    int n = 0;
    if (is_demo_command(cmd, &n)) {
        /* Program task: burst time is the N argument */
        t->burst_time     = n;
        t->remaining_time = n;
        t->is_shell       = 0;
    } else {
        /* Shell command: burst time -1 means "run immediately" */
        t->burst_time     = BURST_SHELL;
        t->remaining_time = BURST_SHELL;
        t->is_shell       = 1;
    }

    /* Assign unique task ID under the queue lock */
    pthread_mutex_lock(&g_queue_mutex);
    t->task_id = g_next_task_id++;
    pthread_mutex_unlock(&g_queue_mutex);

    return t;
}

/* ---------------------------------------------------------------------------
 * scheduler_init() – one-time initialisation
 * --------------------------------------------------------------------------- */
void scheduler_init(void)
{
    g_queue_head   = NULL;
    g_running_task = NULL;
    g_last_run_id  = -1;
    g_next_task_id = 1;
    strncpy(g_timeline, "0)", sizeof(g_timeline) - 1);
    g_timeline_time = 0;
}

/* ---------------------------------------------------------------------------
 * scheduler_add_task() – enqueue a task; signal the scheduler thread
 * --------------------------------------------------------------------------- */
void scheduler_add_task(Task *t)
{
    pthread_mutex_lock(&g_queue_mutex);

    /* Append to tail so that FCFS order is preserved within equal priorities */
    if (!g_queue_head) {
        g_queue_head = t;
    } else {
        Task *cur = g_queue_head;
        while (cur->next) cur = cur->next;
        cur->next = t;
    }

    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);

    log_print("[%d]--- " CLR_BLUE "created" CLR_RESET " (%d)\n", t->client_num, t->burst_time);
}

/* ---------------------------------------------------------------------------
 * queue_remove_task() – unlink and return a specific task from the queue.
 * Caller must hold g_queue_mutex.
 * --------------------------------------------------------------------------- */
static Task *queue_remove_task(int task_id)
{
    Task *prev = NULL;
    Task *cur  = g_queue_head;

    while (cur) {
        if (cur->task_id == task_id) {
            if (prev) prev->next = cur->next;
            else      g_queue_head = cur->next;
            cur->next = NULL;
            return cur;
        }
        prev = cur;
        cur  = cur->next;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * select_next_task() – pick the best candidate from the queue.
 *
 * Rules:
 *   1. Prefer tasks with the shortest remaining_time (SJRF).
 *   2. Do not pick g_last_run_id again if other tasks exist.
 *   3. Tie-break by arrival_time (FCFS).
 *
 * Caller must hold g_queue_mutex.
 * Returns NULL if the queue is empty.
 * --------------------------------------------------------------------------- */
static Task *select_next_task(void)
{
    if (!g_queue_head) return NULL;

    /* Count tasks to enforce anti-consecutive rule */
    int count = 0;
    for (Task *t = g_queue_head; t; t = t->next) count++;

    Task *best = NULL;

    for (Task *t = g_queue_head; t; t = t->next) {
        /* Skip last-run task unless it is the only one left */
        if (count > 1 && t->task_id == g_last_run_id) continue;

        if (!best) { best = t; continue; }

        /* SJRF: pick shorter remaining_time */
        if (t->remaining_time < best->remaining_time) {
            best = t;
        } else if (t->remaining_time == best->remaining_time) {
            /* FCFS tie-break */
            if (t->arrival_time < best->arrival_time) best = t;
        }
    }

    /* If every task was skipped due to anti-consecutive (only 1 task),
     * fall back to picking the head */
    if (!best) best = g_queue_head;

    return best;
}

/* ---------------------------------------------------------------------------
 * run_shell_command() – fork+exec a shell command, stream output to client
 *
 * Shell commands are run synchronously from the scheduler thread.
 * They are never preempted.
 * --------------------------------------------------------------------------- */
static void run_shell_command(Task *t)
{
    log_print("[%d]--- " CLR_GREEN "started" CLR_RESET " (%d)\n", t->client_num, t->burst_time);

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("[ERROR] pipe");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[ERROR] fork");
        close(pipefd[0]); close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        /* Child: redirect stdout+stderr into pipe, then execute */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        Pipeline pl;
        char errbuf[256];
        if (parse_line(t->cmd, &pl, errbuf, sizeof(errbuf)) == 0) {
            execute_pipeline(&pl);
            free_pipeline(&pl);
        } else {
            fprintf(stderr, "%s\n", errbuf);
        }
        exit(0);
    }

    /* Parent: read output from pipe, forward to client socket */
    close(pipefd[1]);

    char buf[4096];
    size_t total = 0;
    ssize_t n;

    /* Collect all output first so we can send the length prefix */
    size_t capacity = 65536;
    char  *out = malloc(capacity);
    if (!out) { close(pipefd[0]); waitpid(pid, NULL, 0); return; }

    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        if (total + (size_t)n >= capacity) {
            capacity *= 2;
            char *tmp = realloc(out, capacity);
            if (!tmp) break;
            out = tmp;
        }
        memcpy(out + total, buf, (size_t)n);
        total += (size_t)n;
    }
    out[total] = '\0';
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    /*
     * Streaming protocol: send [length][bytes] for the output block,
     * then a zero-length terminator so the client knows the command is done.
     */
    uint32_t net_len = htonl((uint32_t)total);
    send_all_fd(t->client_fd, &net_len, sizeof(net_len));
    if (total > 0) send_all_fd(t->client_fd, out, total);

    /* Zero-length terminator */
    uint32_t zero = htonl(0);
    send_all_fd(t->client_fd, &zero, sizeof(zero));

    log_print("[%d]<<< %zu bytes sent\n", t->client_num, total);
    log_print("[%d]--- " CLR_RED "ended" CLR_RESET " (%d)\n", t->client_num, t->burst_time);

    free(out);

    /* Record in timeline */
    timeline_append(t->client_num, 0);
}

/* ---------------------------------------------------------------------------
 * forward_output_for() – read up to max_seconds of output from t->out_pipe[0]
 * and forward each line to t->client_fd.
 *
 * Returns the number of seconds actually consumed (lines read), or -1 if the
 * child process exited before the quantum was exhausted.
 *
 * Implementation: the demo program prints one "Demo i/N\n" line per second.
 * We read line-by-line with a 1.1-second timeout per read so we detect when
 * the child has been SIGSTOP'd (no more output) within roughly one second.
 * --------------------------------------------------------------------------- */
static int forward_output_for(Task *t, int max_seconds)
{
    int fd = t->out_pipe[0];  /* read end of child's stdout pipe */
    int elapsed = 0;

    /*
     * Use select() with a 1.1s timeout to read each line.
     * If select() times out, the child is either stopped or done.
     */
    char line_buf[256];
    int  buf_pos = 0;

    while (elapsed < max_seconds) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 100000 }; /* 1.1 s */
        int ready = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (ready <= 0) {
            /* Timeout: child stopped or finished */
            break;
        }

        /* Read one byte at a time to detect newlines */
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            /* EOF: child exited */
            return -1;
        }

        if (buf_pos < (int)sizeof(line_buf) - 1) {
            line_buf[buf_pos++] = c;
        }

        if (c == '\n') {
            line_buf[buf_pos] = '\0';

            /* Forward as a length-prefixed packet so client can read it */
            uint32_t pkt_len = htonl((uint32_t)buf_pos);
            send_all_fd(t->client_fd, &pkt_len, sizeof(pkt_len));
            send_all_fd(t->client_fd, line_buf, (size_t)buf_pos);

            buf_pos = 0;
            elapsed++;

            log_print("[%d]--- " CLR_YELLOW "running" CLR_RESET " (%d)\n",
                      t->client_num, t->remaining_time - elapsed);

            /* After each completed second, check if a shorter job arrived.
             * If so, break early so the scheduler can preempt this task. */
            if (check_preemption(t)) {
                break;
            }
        }
    }

    return elapsed;
}

/* ---------------------------------------------------------------------------
 * run_program_task() – execute or resume a program task for one quantum
 *
 * First call (child_pid == 0): fork the child, open pipe.
 * Subsequent calls: SIGCONT the stopped child.
 *
 * After quantum seconds (or child exit), either:
 *   - SIGSTOP the child and re-queue it (not done), or
 *   - reap the child and mark it done.
 * --------------------------------------------------------------------------- */
static int run_program_task(Task *t)
{
    int quantum = (t->round == 0) ? QUANTUM_FIRST : QUANTUM_REST;

    if (t->child_pid == 0) {
        /* First execution: fork the demo child */
        if (pipe(t->out_pipe) < 0) {
            perror("[ERROR] pipe for program");
            return -1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("[ERROR] fork for program");
            close(t->out_pipe[0]); close(t->out_pipe[1]);
            return -1;
        }

        if (pid == 0) {
            /* Child: attach stdout+stderr to pipe write end */
            close(t->out_pipe[0]);
            dup2(t->out_pipe[1], STDOUT_FILENO);
            dup2(t->out_pipe[1], STDERR_FILENO);
            close(t->out_pipe[1]);

            /* Build argv for execvp */
            char *argv[64];
            int   argc = 0;
            char  cmd_copy[MAX_CMD];
            strncpy(cmd_copy, t->cmd, MAX_CMD - 1);
            cmd_copy[MAX_CMD - 1] = '\0';

            char *tok = strtok(cmd_copy, " \t");
            while (tok && argc < 63) {
                argv[argc++] = tok;
                tok = strtok(NULL, " \t");
            }
            argv[argc] = NULL;

            execvp(argv[0], argv);
            fprintf(stderr, "Command not found: %s\n", argv[0]);
            exit(1);
        }

        /* Parent */
        close(t->out_pipe[1]);  /* parent only reads */
        t->child_pid = pid;
        log_print("[%d]--- " CLR_GREEN "started" CLR_RESET " (%d)\n", t->client_num, t->remaining_time);
    } else {
        /* Resume a previously stopped child */
        kill(t->child_pid, SIGCONT);
        log_print("[%d]--- " CLR_YELLOW "running" CLR_RESET " (%d)\n", t->client_num, t->remaining_time);
    }

    t->round++;
    t->state = STATE_RUNNING;

    /* Run for up to quantum seconds, forwarding output line by line */
    int elapsed = forward_output_for(t, quantum);

    if (elapsed < 0) {
        /* Child exited naturally during this quantum */
        int status;
        waitpid(t->child_pid, &status, 0);
        t->child_pid      = 0;
        t->remaining_time = 0;
        close(t->out_pipe[0]);
        t->out_pipe[0] = -1;

        /* Send the EOF marker (zero-length length prefix) so the client knows
         * the program finished streaming */
        uint32_t zero = htonl(0);
        send_all_fd(t->client_fd, &zero, sizeof(zero));

        log_print("[%d]--- " CLR_RED "ended" CLR_RESET " (0)\n", t->client_num);
        return 1;  /* done */
    }

    /* Quantum expired (or preempted): stop the child */
    t->remaining_time -= elapsed;
    kill(t->child_pid, SIGSTOP);

    if (t->remaining_time <= 0) {
        /* No work left: reap child and mark done */
        kill(t->child_pid, SIGCONT);  /* allow child to exit cleanly */
        kill(t->child_pid, SIGTERM);
        int status;
        waitpid(t->child_pid, &status, WNOHANG);
        t->child_pid      = 0;
        t->remaining_time = 0;
        close(t->out_pipe[0]);
        t->out_pipe[0] = -1;

        uint32_t zero = htonl(0);
        send_all_fd(t->client_fd, &zero, sizeof(zero));

        log_print("[%d]--- " CLR_RED "ended" CLR_RESET " (0)\n", t->client_num);
        return 1;  /* done */
    }

    log_print("[%d]--- " CLR_CYAN "waiting" CLR_RESET " (%d)\n", t->client_num, t->remaining_time);
    return 0;  /* not done, re-queue */
}

/* ---------------------------------------------------------------------------
 * check_preemption() – called while a program task is running.
 *
 * If any queued task has strictly shorter remaining_time than the running
 * task AND the running task has used at least 1 second, return 1 so the
 * scheduler knows to stop the current task early.
 *
 * Caller must NOT hold g_queue_mutex (this function acquires it).
 * --------------------------------------------------------------------------- */
static int check_preemption(Task *running)
{
    pthread_mutex_lock(&g_queue_mutex);
    int should_preempt = 0;

    for (Task *t = g_queue_head; t; t = t->next) {
        if (t->task_id == running->task_id) continue;
        if (t->remaining_time < running->remaining_time) {
            should_preempt = 1;
            break;
        }
    }

    pthread_mutex_unlock(&g_queue_mutex);
    return should_preempt;
}

/* ---------------------------------------------------------------------------
 * scheduler_remove_client_tasks() – remove all tasks for a disconnected client
 * --------------------------------------------------------------------------- */
void scheduler_remove_client_tasks(int client_num)
{
    pthread_mutex_lock(&g_queue_mutex);

    Task *prev = NULL;
    Task *cur  = g_queue_head;

    while (cur) {
        if (cur->client_num == client_num) {
            /* Kill child if running */
            if (cur->child_pid > 0) {
                kill(cur->child_pid, SIGKILL);
                waitpid(cur->child_pid, NULL, WNOHANG);
            }
            if (cur->out_pipe[0] >= 0) close(cur->out_pipe[0]);

            Task *to_free = cur;
            if (prev) prev->next = cur->next;
            else      g_queue_head = cur->next;
            cur = cur->next;
            free(to_free);
        } else {
            prev = cur;
            cur  = cur->next;
        }
    }

    /* Also kill running task if it belongs to this client */
    if (g_running_task && g_running_task->client_num == client_num) {
        if (g_running_task->child_pid > 0) {
            kill(g_running_task->child_pid, SIGKILL);
        }
    }

    pthread_mutex_unlock(&g_queue_mutex);
}

/* ---------------------------------------------------------------------------
 * scheduler_thread() – the main scheduling loop
 *
 * Waits for tasks in the queue, selects one via SJRF, runs it for a quantum,
 * and either re-queues or removes it based on completion.
 * --------------------------------------------------------------------------- */
static void *scheduler_thread(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&g_queue_mutex);

        /* Wait until there is at least one task in the queue */
        while (!g_queue_head) {
            pthread_cond_wait(&g_queue_cond, &g_queue_mutex);
        }

        /* Select the best task */
        Task *chosen = select_next_task();
        if (!chosen) {
            pthread_mutex_unlock(&g_queue_mutex);
            continue;
        }

        /* Remove it from the queue so it is not selected by a concurrent reader */
        queue_remove_task(chosen->task_id);
        g_running_task = chosen;
        chosen->state  = STATE_RUNNING;

        pthread_mutex_unlock(&g_queue_mutex);

        /* ---- Execute the chosen task ---- */
        if (chosen->is_shell) {
            /* Shell commands run fully without preemption */
            run_shell_command(chosen);
            timeline_append(chosen->client_num, 0);
            g_last_run_id = chosen->task_id;
            free(chosen);
        } else {
            /* Program task: run for one quantum */
            int done = run_program_task(chosen);
            int elapsed = (chosen->round == 1) ? QUANTUM_FIRST : QUANTUM_REST;
            timeline_append(chosen->client_num, elapsed);

            g_last_run_id = chosen->task_id;

            if (done) {
                free(chosen);
            } else {
                /* Re-enqueue for further execution */
                pthread_mutex_lock(&g_queue_mutex);
                chosen->state = STATE_WAITING;

                /* Append at tail to preserve FCFS among equal-priority tasks */
                if (!g_queue_head) {
                    g_queue_head = chosen;
                    chosen->next = NULL;
                } else {
                    Task *tail = g_queue_head;
                    while (tail->next) tail = tail->next;
                    tail->next  = chosen;
                    chosen->next = NULL;
                }

                pthread_cond_signal(&g_queue_cond);
                pthread_mutex_unlock(&g_queue_mutex);
            }
        }

        pthread_mutex_lock(&g_queue_mutex);
        g_running_task = NULL;
        pthread_mutex_unlock(&g_queue_mutex);
    }

    return NULL;
}

/* ---------------------------------------------------------------------------
 * scheduler_start() – spawn the scheduler thread
 * --------------------------------------------------------------------------- */
int scheduler_start(void)
{
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, scheduler_thread, NULL);
    if (rc != 0) return rc;
    pthread_detach(tid);
    return 0;
}

/* ---------------------------------------------------------------------------
 * scheduler_print_timeline() – print the execution summary
 * --------------------------------------------------------------------------- */
void scheduler_print_timeline(void)
{
    pthread_mutex_lock(&g_print_mutex);
    printf("\n%s\n", g_timeline);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mutex);
}
