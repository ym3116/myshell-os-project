/* =============================================================================
 * src/pipe.c  –  Anonymous-pipe creation, cleanup, and per-child wiring
 *
 * Responsibility (Person B):
 *   Provides three functions used by execute_pipeline() in exec.c to manage
 *   the array of Unix pipes that connect adjacent commands in a pipeline.
 *
 * How pipes map onto a pipeline:
 *
 *   cmd0  |  cmd1  |  cmd2
 *       pipe[0]  pipe[1]
 *
 *   pipe[i][0] = read  end  (connected to cmd[i+1]'s stdin)
 *   pipe[i][1] = write end  (connected to cmd[i]'s stdout)
 *
 *   For n commands we need exactly n-1 pipes.
 *
 * Ordering contract (enforced by exec.c):
 *   1. connect_pipes_for_child() runs first in each child – it installs the
 *      correct pipe ends onto STDIN_FILENO / STDOUT_FILENO via dup2().
 *   2. apply_redirections() runs second – explicit '<' or '>' operators
 *      override the pipe connections, giving them higher priority.
 *   3. close_all_pipes() is called in the parent after all children are
 *      forked – keeping the parent's copies open would prevent EOF.
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <unistd.h>     /* pipe(), dup2(), close() */
#include <stdio.h>      /* perror() */

#include "exec.h"       /* function declarations */


/* -----------------------------------------------------------------------------
 * create_pipes()
 *
 * Creates n_pipes anonymous pipes and stores each one in pipe_fds[i][2].
 *
 * Pipes are created sequentially.  If the i-th call to pipe(2) fails, all
 * previously opened pipes are closed before returning -1, so no file
 * descriptors are leaked to the caller.
 *
 * Parameters:
 *   n_pipes   – number of pipes to create (= n_cmds - 1 for a pipeline)
 *   pipe_fds  – caller-allocated array of [n_pipes][2] ints;
 *               on success pipe_fds[i][0] = read end,
 *                          pipe_fds[i][1] = write end
 *
 * Returns:
 *    0  – all n_pipes pipes were created successfully
 *   -1  – a pipe() call failed (errno set, message printed to stderr)
 * ----------------------------------------------------------------------------- */
int create_pipes(int n_pipes, int (*pipe_fds)[2])
{
    for (int i = 0; i < n_pipes; i++) {
        if (pipe(pipe_fds[i]) < 0) {
            perror("pipe");

            /* Clean up every pipe that was successfully opened before this
             * failure so no file descriptors leak out of this function. */
            for (int j = 0; j < i; j++) {
                close(pipe_fds[j][0]);
                close(pipe_fds[j][1]);
            }
            return -1;
        }
    }
    return 0;
}


/* -----------------------------------------------------------------------------
 * close_all_pipes()
 *
 * Closes both the read and write ends of every pipe in pipe_fds[0..n_pipes-1].
 *
 * There are two distinct callers and two distinct reasons:
 *
 *   Parent process (after forking all children):
 *     The parent inherited copies of all pipe ends when it called pipe().
 *     If the parent does not close its write ends, a child that reads from
 *     the corresponding read end will never see EOF – it blocks forever
 *     waiting for data that will never arrive.
 *
 *   Child process (inside connect_pipes_for_child, after dup2):
 *     Once dup2() has installed the one or two pipe ends the child actually
 *     needs onto STDIN_FILENO / STDOUT_FILENO, all remaining raw pipe fds
 *     must be closed so they are not inherited by exec'd programs.
 *
 * Parameters:
 *   n_pipes   – number of pipes in the array
 *   pipe_fds  – the same array passed to create_pipes()
 * ----------------------------------------------------------------------------- */
void close_all_pipes(int n_pipes, int (*pipe_fds)[2])
{
    for (int i = 0; i < n_pipes; i++) {
        close(pipe_fds[i][0]);   /* close read end  */
        close(pipe_fds[i][1]);   /* close write end */
    }
}


/* -----------------------------------------------------------------------------
 * connect_pipes_for_child()
 *
 * Called inside a child process to wire the correct pipe ends onto the
 * standard file descriptors before execvp().
 *
 * Layout for  cmd0 | cmd1 | cmd2  (n_cmds=3, n_pipes=2):
 *
 *   cmd_idx = 0  (first):   stdout → pipe_fds[0][1]
 *   cmd_idx = 1  (middle):  stdin  → pipe_fds[0][0]
 *                           stdout → pipe_fds[1][1]
 *   cmd_idx = 2  (last):    stdin  → pipe_fds[1][0]
 *
 * After the dup2() calls close_all_pipes() is invoked to discard every raw
 * pipe fd in the child; the child only needs the inherited STDIN/STDOUT.
 *
 * Important: this function is called BEFORE apply_redirections(), so that
 * any explicit '< file' or '> file' tokens in the same command override the
 * pipe connections installed here.  For example, in
 *
 *   cat < input.txt | grep foo
 *
 * cat's stdin is connected to pipe[0][0] by this function, but then
 * apply_redirections() replaces STDIN_FILENO with input.txt.
 *
 * Parameters:
 *   cmd_idx  – zero-based index of this command in the pipeline
 *   n_cmds   – total number of commands in the pipeline
 *   n_pipes  – number of pipes (= n_cmds - 1)
 *   pipe_fds – the pipe array created by create_pipes()
 * ----------------------------------------------------------------------------- */
void connect_pipes_for_child(int cmd_idx, int n_cmds,
                             int n_pipes, int (*pipe_fds)[2])
{
    /* Connect stdin to the READ end of the previous pipe.
     * Not applicable for the first command (cmd_idx == 0) because it has
     * no upstream neighbour; it reads from the terminal (or a '<' file). */
    if (cmd_idx > 0) {
        /* pipe_fds[cmd_idx - 1][0] is the read end that delivers the previous
         * command's output to this command's standard input. */
        if (dup2(pipe_fds[cmd_idx - 1][0], STDIN_FILENO) < 0) {
            perror("dup2: pipe stdin");
        }
    }

    /* Connect stdout to the WRITE end of the next pipe.
     * Not applicable for the last command (cmd_idx == n_cmds - 1) because it
     * has no downstream neighbour; its output goes to the terminal (or '>').  */
    if (cmd_idx < n_cmds - 1) {
        /* pipe_fds[cmd_idx][1] is the write end that carries this command's
         * output to the next command's standard input. */
        if (dup2(pipe_fds[cmd_idx][1], STDOUT_FILENO) < 0) {
            perror("dup2: pipe stdout");
        }
    }

    /* Close all raw pipe fds in this child.  We have already dup2'd the ones
     * we need onto STDIN_FILENO / STDOUT_FILENO; keeping the others open
     * would confuse the kernel's reference counting and prevent proper EOF. */
    close_all_pipes(n_pipes, pipe_fds);
}
