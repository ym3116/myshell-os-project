/* =============================================================================
 * src/exec.c  –  Pipeline execution engine
 * Each child process:
 *     a. Calls connect_pipes_for_child()  – installs pipe ends on STDIN/STDOUT
 *     b. Calls apply_redirections()       – overrides with explicit < > 2> files
 *     c. Calls execvp()                   – replaces itself with the real program
 *
 * Error handling (runtime, after successful parse):
 *   "File not found."                      – open() failed for an input file
 *                                            (printed inside apply_redirections)
 *   "Command not found."                   – execvp() failed, single command
 *   "Command not found in pipe sequence."  – execvp() failed, multiple commands
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>      // perror(), fprintf()
#include <stdlib.h>     // malloc(), free(), exit()
#include <unistd.h>     // fork(), execvp(), dup2(), close()
#include <sys/wait.h>   // waitpid(), WIFEXITED, WEXITSTATUS
#include "exec.h"       


int execute_pipeline(const Pipeline *p)
{
    /* Guard against NULL or empty pipeline */
    if (p == NULL || p->n_cmds == 0) return 0;

    int n_cmds  = p->n_cmds;
    int n_pipes = n_cmds - 1;   /* one pipe per adjacent command pair */

    /* ------------------------------------------------------------------
     * Step 1 – Create n_pipes anonymous pipes.
     *
     * We allocate the pipe array on the heap so the function supports
     * arbitrarily long pipelines (not capped by stack size).
     * ------------------------------------------------------------------ */
    int (*pipe_fds)[2] = NULL;

    if (n_pipes > 0) {
        /* Allocate space for n_pipes pairs of file descriptors */
        pipe_fds = malloc((size_t)n_pipes * sizeof(int[2]));
        if (pipe_fds == NULL) {
            perror("malloc (pipe_fds)");
            return -1;
        }

        /* create_pipes() opens all n_pipes pipes; on failure it cleans up
         * any that were partially opened and prints an error. */
        if (create_pipes(n_pipes, pipe_fds) < 0) {
            free(pipe_fds);
            return -1;
        }
    }

    /* ------------------------------------------------------------------
     * Allocate PID array so we can wait for every child in Step 4.
     * ------------------------------------------------------------------ */
    pid_t *pids = malloc((size_t)n_cmds * sizeof(pid_t));
    if (pids == NULL) {
        perror("malloc (pids)");
        if (pipe_fds) { close_all_pipes(n_pipes, pipe_fds); free(pipe_fds); }
        return -1;
    }

    /* ------------------------------------------------------------------
     * Step 2 – Fork one child per command.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < n_cmds; i++) {

        pid_t pid = fork();

        if (pid < 0) {
            /* fork() itself failed (e.g. EAGAIN, ENOMEM).
             * Close all open pipe ends so nothing leaks, then wait for
             * any children already spawned to avoid zombie processes. */
            perror("fork");
            if (pipe_fds) close_all_pipes(n_pipes, pipe_fds);
            for (int j = 0; j < i; j++) waitpid(pids[j], NULL, 0);
            free(pids);
            if (pipe_fds) free(pipe_fds);
            return -1;
        }

        if (pid == 0) {
            /* ============================================================
             * CHILD PROCESS
             * ============================================================ */

            // Pipe connections
            if (n_pipes > 0) {
                connect_pipes_for_child(i, n_cmds, n_pipes, pipe_fds);
            }

            // Redirections
            if (apply_redirections(&p->cmds[i]) < 0) {
                /* apply_redirections already printed the error message */
                exit(1);
            }

            // Execution
            execvp(p->cmds[i].argv[0], p->cmds[i].argv);

            if (n_cmds == 1) {
                // Single command case
                fprintf(stderr, "Command not found.\n");
            } else {
                // Multiple commands in a pipeline
                fprintf(stderr, "Command not found in pipe sequence.\n");
            }

            // Conventional exit code for “command not found.”
            exit(127);
        }

        /* ==============================================================
         * PARENT PROCESS – record child PID and continue forking
         * ============================================================== */
        pids[i] = pid;
    }

    /* ------------------------------------------------------------------
     * Step 3 – Parent closes all pipe ends.
     * ------------------------------------------------------------------ */
    if (pipe_fds) {
        close_all_pipes(n_pipes, pipe_fds);
        free(pipe_fds);         /* heap memory no longer needed */
    }

    /* ------------------------------------------------------------------
     * Step 4 – Wait for all child processes.
     * ------------------------------------------------------------------ */
    int last_exit = 0;

    for (int i = 0; i < n_cmds; i++) {
        int status;
        waitpid(pids[i], &status, 0);   /* block until child i exits */

        /* Capture the numeric exit code of the last command */
        if (i == n_cmds - 1) {
            if (WIFEXITED(status)) {
                last_exit = WEXITSTATUS(status);
            } else {
                /* Child was killed by a signal; treat as failure */
                last_exit = 1;
            }
        }
    }

    free(pids);
    return last_exit;
}
