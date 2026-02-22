/* =============================================================================
 * src/exec.c  –  Pipeline execution engine
 *
 * Responsibility (Person B):
 *   Implements execute_pipeline(), the single entry point called by main()
 *   once parse_line() has validated the user's input and populated a Pipeline
 *   struct.  This file orchestrates the three OS-level phases:
 *
 *     1. Pipe setup   – create n_cmds-1 anonymous pipes   (pipe.c helpers)
 *     2. Forking      – spawn one child per command        (fork / execvp)
 *     3. Waiting      – reap every child before re-prompting (waitpid)
 *
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
 *
 * Waiting contract:
 *   The shell must not re-display the prompt until every child in the pipeline
 *   has terminated.  We therefore call waitpid() for EACH forked child, not
 *   just the last one.  This also prevents zombie processes.
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>      /* fprintf(), perror()                    */
#include <stdlib.h>     /* malloc(), free(), exit()               */
#include <unistd.h>     /* fork(), execvp(), close()              */
#include <sys/wait.h>   /* waitpid(), WIFEXITED(), WEXITSTATUS()  */

#include "exec.h"       /* execute_pipeline() + helper declarations */


/* -----------------------------------------------------------------------------
 * execute_pipeline()
 *
 * Executes a validated Pipeline of one or more Commands.
 *
 * Algorithm overview
 * ------------------
 * Let n = p->n_cmds (number of commands).
 *
 *   Step 1 – Allocate n-1 pipes.
 *             Each pipe[i] connects cmd[i]'s stdout to cmd[i+1]'s stdin.
 *             A single command (n=1) requires zero pipes.
 *
 *   Step 2 – Fork n children.
 *             For child i:
 *               a. connect_pipes_for_child() uses dup2() to attach the
 *                  relevant pipe ends to STDIN_FILENO / STDOUT_FILENO, then
 *                  closes all raw pipe fds in the child.
 *               b. apply_redirections() opens any <, >, or 2> files and
 *                  uses dup2() to override the standard descriptors.
 *                  Explicit redirections override pipe connections because
 *                  this step runs AFTER step (a).
 *               c. execvp() replaces the child with the real program.
 *                  If execvp returns, the command was not found.
 *             The parent records each child's PID in a pids[] array.
 *
 *   Step 3 – Parent closes all pipe ends.
 *             Once all children are forked the parent must close its copies
 *             of every pipe fd.  If it does not, readers will never see EOF.
 *
 *   Step 4 – Parent waits for all children.
 *             We call waitpid() for every PID collected in step 2.
 *             The return value reflects the last command's exit status,
 *             mirroring the convention used by real Unix shells.
 *
 * Parameters:
 *   p  – pointer to a fully-validated Pipeline (must not be NULL)
 *
 * Returns:
 *    0  – pipeline ran and the last command exited successfully
 *   >0  – exit status of the last command
 *   -1  – a system call in the parent failed (malloc, pipe, fork)
 * ----------------------------------------------------------------------------- */
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

            /* --- (a) Wire pipe ends onto STDIN / STDOUT via dup2() ------
             * connect_pipes_for_child() figures out which pipe ends this
             * child needs based on its position in the pipeline:
             *   first cmd  → only stdout → pipe write end
             *   middle cmd → stdin ← pipe read end AND stdout → pipe write end
             *   last cmd   → only stdin ← pipe read end
             * After dup2 it closes every raw pipe fd in this child.       */
            if (n_pipes > 0) {
                connect_pipes_for_child(i, n_cmds, n_pipes, pipe_fds);
            }

            /* --- (b) Apply explicit file redirections (< > 2>) ----------
             * apply_redirections() checks in_file, out_file, err_file on
             * the Command struct.  If present it opens the file and dup2s
             * the fd onto the appropriate standard descriptor.
             *
             * IMPORTANT: this runs AFTER connect_pipes_for_child(), so an
             * explicit '< file' overrides a pipe that was just installed on
             * STDIN_FILENO.  This is the correct precedence.             */
            if (apply_redirections(&p->cmds[i]) < 0) {
                /* apply_redirections already printed the error message */
                exit(1);
            }

            /* --- (c) Execute the command --------------------------------
             * execvp() searches PATH for argv[0] automatically, so
             * both bare commands ("ls") and path-qualified ones ("./hello")
             * work without special-casing.
             *
             * argv is NULL-terminated as required by execvp's signature.  */
            execvp(p->cmds[i].argv[0], p->cmds[i].argv);

            /* If execvp() returns, the command was not found or could not
             * be launched.  Print the spec-required error message:
             *   single command   → "Command not found."
             *   inside pipeline  → "Command not found in pipe sequence."  */
            if (n_cmds == 1) {
                fprintf(stderr, "Command not found.\n");
            } else {
                fprintf(stderr, "Command not found in pipe sequence.\n");
            }

            /* Exit with 127 – the conventional "command not found" code
             * used by bash/dash so callers can detect it if needed.      */
            exit(127);
        }

        /* ==============================================================
         * PARENT PROCESS – record child PID and continue forking
         * ============================================================== */
        pids[i] = pid;
    }

    /* ------------------------------------------------------------------
     * Step 3 – Parent closes all pipe ends.
     *
     * Now that every child has been forked and has its own copies of the
     * pipe fds, the parent must release its copies.  If the parent kept
     * a write end open, the corresponding reader child would never see
     * EOF (the kernel keeps a pipe open as long as ANY process holds a
     * write-end fd) and would block indefinitely.
     * ------------------------------------------------------------------ */
    if (pipe_fds) {
        close_all_pipes(n_pipes, pipe_fds);
        free(pipe_fds);         /* heap memory no longer needed */
    }

    /* ------------------------------------------------------------------
     * Step 4 – Wait for all child processes.
     *
     * The spec requires waiting for the last process before re-prompting,
     * but we wait for ALL of them to avoid leaving zombies and to ensure
     * that any output buffered in intermediate processes is fully flushed
     * to disk / the terminal before the next prompt appears.
     *
     * We track the exit status of the LAST command (index n_cmds-1) to
     * mirror real shell behaviour where "$?" reflects the rightmost
     * command in a pipeline.
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
