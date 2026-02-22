#ifndef EXEC_H
#define EXEC_H

#include "parser.h"

/* =========================================================================
 * exec.h  –  Public interface for the execution engine (Person B)
 *
 * This header declares all functions implemented across three source files:
 *   exec.c  – top-level pipeline orchestration (fork, wait, execvp)
 *   redir.c – file-descriptor redirection via dup2()
 *   pipe.c  – pipe creation, cleanup, and per-child pipe wiring
 * ========================================================================= */


/* -------------------------------------------------------------------------
 * execute_pipeline()                                            (exec.c)
 *
 * Entry point called by main() after a successful parse.
 * Given a validated Pipeline struct, it:
 *   1. Creates n_cmds-1 pipes for inter-process communication.
 *   2. Forks one child process per command in the pipeline.
 *   3. In each child: wires pipe ends then applies file redirections,
 *      then calls execvp() to replace the child image with the command.
 *   4. In the parent: closes all pipe ends and waits for every child.
 *
 * Returns:
 *   0          – all commands ran and the last one exited with status 0
 *   nonzero    – a child's exit code, or -1 on a system-call failure
 *
 * Runtime errors printed to stderr:
 *   "File not found."                   – open() failed on an input file
 *   "Command not found."                – execvp() failed, single command
 *   "Command not found in pipe sequence." – execvp() failed inside a pipe
 * ------------------------------------------------------------------------- */
int execute_pipeline(const Pipeline *p);


/* -------------------------------------------------------------------------
 * apply_redirections()                                          (redir.c)
 *
 * Called inside each child process (after fork, before execvp).
 * Inspects the Command's in_file / out_file / err_file fields and, for
 * each non-NULL field, opens the file and uses dup2() to replace the
 * corresponding standard file descriptor:
 *
 *   in_file  → open(O_RDONLY)              → dup2(fd, STDIN_FILENO)
 *   out_file → open(O_WRONLY|O_CREAT|O_TRUNC, 0644) → dup2(fd, STDOUT_FILENO)
 *   err_file → open(O_WRONLY|O_CREAT|O_TRUNC, 0644) → dup2(fd, STDERR_FILENO)
 *
 * The original fd returned by open() is closed after dup2() so it does not
 * leak into the exec'd program.
 *
 * Returns:
 *    0  – all requested redirections succeeded
 *   -1  – a redirection failed (error message already printed to stderr)
 * ------------------------------------------------------------------------- */
int apply_redirections(const Command *cmd);


/* -------------------------------------------------------------------------
 * create_pipes()                                                (pipe.c)
 *
 * Allocates n_pipes anonymous Unix pipes.  Each pipe is stored as a
 * two-element int array:  pipe_fds[i][0] = read end,
 *                         pipe_fds[i][1] = write end.
 *
 * If any pipe() call fails the function closes all previously opened
 * pipes before returning.
 *
 * Returns:
 *    0  – all n_pipes pipes created successfully
 *   -1  – pipe() system call failed (errno set, message printed)
 * ------------------------------------------------------------------------- */
int create_pipes(int n_pipes, int (*pipe_fds)[2]);


/* -------------------------------------------------------------------------
 * close_all_pipes()                                             (pipe.c)
 *
 * Closes every read and write end in pipe_fds[0..n_pipes-1].
 *
 * Must be called in the PARENT process after all children have been
 * forked.  Leaving pipe ends open in the parent would prevent readers
 * from ever seeing EOF, causing them to block indefinitely.
 *
 * Also called inside each child (via connect_pipes_for_child) to discard
 * the raw pipe fds once dup2() has installed the needed ends onto
 * STDIN_FILENO / STDOUT_FILENO.
 * ------------------------------------------------------------------------- */
void close_all_pipes(int n_pipes, int (*pipe_fds)[2]);


/* -------------------------------------------------------------------------
 * connect_pipes_for_child()                                     (pipe.c)
 *
 * Called inside a child process to wire the correct pipe ends onto the
 * standard input / output file descriptors before execvp().
 *
 * For a pipeline  cmd0 | cmd1 | cmd2  the pipes are numbered 0 and 1:
 *   pipe_fds[0] carries cmd0's stdout  →  cmd1's stdin
 *   pipe_fds[1] carries cmd1's stdout  →  cmd2's stdin
 *
 * Rules applied by cmd_idx:
 *   First command  (cmd_idx == 0):
 *       stdout → dup2(pipe_fds[0][1], STDOUT_FILENO)
 *   Middle command (0 < cmd_idx < n_cmds-1):
 *       stdin  → dup2(pipe_fds[cmd_idx-1][0], STDIN_FILENO)
 *       stdout → dup2(pipe_fds[cmd_idx][1],   STDOUT_FILENO)
 *   Last command   (cmd_idx == n_cmds-1):
 *       stdin  → dup2(pipe_fds[cmd_idx-1][0], STDIN_FILENO)
 *
 * After all dup2() calls, close_all_pipes() is invoked to discard the
 * raw fds – the child only needs the inherited STDIN/STDOUT descriptors.
 *
 * Note: explicit file redirections (<, >, 2>) are applied AFTER this
 * function returns (in apply_redirections), so they correctly override
 * the pipe connections when both appear in the same command.
 * ------------------------------------------------------------------------- */
void connect_pipes_for_child(int cmd_idx, int n_cmds,
                             int n_pipes, int (*pipe_fds)[2]);

#endif /* EXEC_H */
