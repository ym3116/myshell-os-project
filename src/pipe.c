/* =============================================================================
 * src/pipe.c 
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
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <unistd.h>     // pipe(), dup2(), close()
#include <stdio.h>      // perror()
#include "exec.h"       


/* -----------------------------------------------------------------------------
 * create_pipes()
 *
 * Creates n_pipes anonymous pipes and stores each one in pipe_fds[i][2].
 *
 * If the i-th call to pipe(2) fails, all
 * previously opened pipes are closed before returning -1, so no file
 * descriptors are leaked to the caller.
 *
 * Parameters:
 *   n_pipes   – number of pipes to create (= n_cmds - 1 for a pipeline)
 *   pipe_fds  – pointer to an array of 2 ints:
 *               on success pipe_fds[i][0] = read end,
 *                          pipe_fds[i][1] = write end
 * ----------------------------------------------------------------------------- */
int create_pipes(int n_pipes, int (*pipe_fds)[2])
{
    for (int i = 0; i < n_pipes; i++) {
        if (pipe(pipe_fds[i]) < 0) {
            perror("pipe"); 

            // Clean up every pipe before this failure (if one fails, all fail)
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
 * Important: this function is called BEFORE apply_redirections(), so that
 * any explicit '< file' or '> file' tokens in the same command override the
 * pipe connections installed here.  For example, in
 *
 *   cat < input.txt | grep foo
 *
 * cat's stdin is connected to pipe[0][0] by this function, but then
 * apply_redirections() replaces STDIN_FILENO with input.txt.
 * ----------------------------------------------------------------------------- */
void connect_pipes_for_child(int cmd_idx, int n_cmds,
                             int n_pipes, int (*pipe_fds)[2])
{
    // Connect stdin to the READ end of previous pipe
    if (cmd_idx > 0) {
        if (dup2(pipe_fds[cmd_idx - 1][0], STDIN_FILENO) < 0) {
            perror("dup2: pipe stdin");
        }
    }

    // Connect stdout to the WRITE end of the next pipe.
    if (cmd_idx < n_cmds - 1) {
        if (dup2(pipe_fds[cmd_idx][1], STDOUT_FILENO) < 0) {
            perror("dup2: pipe stdout");
        }
    }

    close_all_pipes(n_pipes, pipe_fds);
}
