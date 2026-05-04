/* =============================================================================
 * include/scheduler.h  –  Phase 4 Scheduler Interface
 *
 * Defines the Task struct and the public API for the scheduler module.
 * The scheduler runs as a dedicated thread, owns the waiting queue, and
 * implements a combined SJRF + Round-Robin algorithm with selective preemption.
 *
 * Algorithm summary:
 *   - Shell commands (burst_time == BURST_SHELL):  run immediately, never
 *     preempted, never queued.
 *   - Program tasks: queued, selected by shortest remaining_time (SJRF),
 *     given a time quantum (QUANTUM_FIRST for round 1, QUANTUM_REST after),
 *     preempted via SIGSTOP if a shorter job arrives, resumed via SIGCONT.
 *   - Tie-break: FCFS (arrival order).
 *   - Anti-consecutive: the same task cannot be picked twice in a row unless
 *     it is the only task in the queue.
 * ============================================================================= */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <pthread.h>
#include <sys/types.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Scheduling constants
 * --------------------------------------------------------------------------- */
#define BURST_SHELL      (-1)   /* burst_time assigned to shell commands        */
#define DEFAULT_BURST    10     /* default burst for unknown programs           */
#define QUANTUM_FIRST     3     /* seconds for a task's very first round        */
#define QUANTUM_REST      7     /* seconds for all subsequent rounds            */
#define MAX_CMD         4096    /* maximum command string length                */
#define MAX_TASKS        256    /* maximum simultaneous tasks in the queue      */

/* ---------------------------------------------------------------------------
 * Task states (used for server-side logging)
 * --------------------------------------------------------------------------- */
typedef enum {
    STATE_WAITING = 0,  /* in queue, not running           */
    STATE_RUNNING,      /* currently executing on the CPU  */
    STATE_DONE          /* finished, about to be removed   */
} TaskState;

/* ---------------------------------------------------------------------------
 * Task
 *
 * One entry in the scheduler's waiting queue.  Fields filled by the client
 * thread when the command is received; the scheduler thread updates
 * remaining_time, round, state, and child_pid as execution proceeds.
 * --------------------------------------------------------------------------- */
typedef struct Task {
    int        task_id;          /* unique monotonic ID                        */
    int        client_fd;        /* socket to send output back to the client   */
    int        client_num;       /* human-readable client identifier (#N)      */
    char       cmd[MAX_CMD];     /* original command string                    */
    int        burst_time;       /* original N; BURST_SHELL for shell cmds     */
    int        remaining_time;   /* iterations still to run                    */
    int        round;            /* how many rounds this task has had so far   */
    int        is_shell;         /* 1 = shell command, 0 = program             */
    pid_t      child_pid;        /* child process PID (0 if not yet started)   */
    int        out_pipe[2];      /* pipe: child stdout/stderr → scheduler      */
    TaskState  state;            /* current scheduling state                   */
    time_t     arrival_time;     /* for FCFS tie-breaking                      */
    size_t     total_bytes_sent; /* total output bytes streamed to client      */
    struct Task *next;           /* intrusive linked-list pointer              */
} Task;

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/*
 * scheduler_init() – Must be called once before any other scheduler function.
 * Initialises the queue, mutex, condition variable, and task-ID counter.
 */
void scheduler_init(void);

/*
 * scheduler_start() – Spawns the scheduler thread.
 * Returns 0 on success, nonzero on pthread_create failure.
 */
int  scheduler_start(void);

/*
 * scheduler_add_task() – Called by a client thread to enqueue a new task.
 * Takes ownership of the heap-allocated Task; the caller must not free it.
 */
void scheduler_add_task(Task *t);

/*
 * scheduler_remove_client_tasks() – Removes all queued tasks belonging to
 * client_num from the waiting queue (called when a client disconnects).
 * Tasks that are currently running will be killed via SIGKILL.
 */
void scheduler_remove_client_tasks(int client_num);

/*
 * task_create() – Allocates and initialises a new Task from a command string.
 * Detects whether the command is a shell command or a demo/program invocation,
 * sets burst_time accordingly, and sets state to STATE_WAITING.
 *
 * Parameters:
 *   cmd        – null-terminated command string from the client
 *   client_fd  – connected socket file descriptor
 *   client_num – client identifier for logging
 *
 * Returns a heap-allocated Task on success, NULL on allocation failure.
 */
Task *task_create(const char *cmd, int client_fd, int client_num);

/*
 * send_all_fd() – Utility: guarantees exactly len bytes are written to fd.
 * Returns 0 on success, -1 on error. Defined in scheduler.c, used by server.c.
 */
int  send_all_fd(int fd, const void *buf, size_t len);

/*
 * scheduler_print_timeline() – prints the execution summary line to stdout.
 * Call this when the server shuts down or at end of a test session.
 */
void scheduler_print_timeline(void);

/* Shared print mutex – server.c and scheduler.c both use this for log lines */
extern pthread_mutex_t g_print_mutex;

#endif /* SCHEDULER_H */
