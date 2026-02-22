/* =============================================================================
 * src/redir.c  –  File-descriptor redirection helpers
 *
 * Responsibility (Person B):
 *   Implements apply_redirections(), which is called inside each child
 *   process after fork() but before execvp().  It translates the in_file,
 *   out_file, and err_file fields of a Command struct into actual file-
 *   descriptor operations using open(2) and dup2(2).
 *
 * Design notes:
 *   - Only fields that are non-NULL trigger a redirection; NULL means
 *     "use the inherited descriptor", i.e. no change.
 *   - dup2() atomically replaces the target descriptor (STDIN_FILENO,
 *     STDOUT_FILENO, STDERR_FILENO) with a duplicate of the opened fd.
 *     The original fd from open() is closed immediately after dup2()
 *     so it does not leak into the exec'd program.
 *   - Output files are created if they do not exist and truncated to zero
 *     length if they do (matching standard shell '>' semantics).
 *   - All error messages go to stderr and use the exact phrasing required
 *     by the project specification.
 * ============================================================================= */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>      /* open(), O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC */
#include <unistd.h>     /* dup2(), close(), STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO */
#include <stdio.h>      /* fprintf(), perror() */

#include "exec.h"       /* apply_redirections() declaration + Command typedef */


/* -----------------------------------------------------------------------------
 * apply_redirections()
 *
 * Sets up the three possible file-descriptor redirections for one command:
 *
 *   Input  redirection  (<)  : cmd->in_file  → STDIN_FILENO
 *   Output redirection  (>)  : cmd->out_file → STDOUT_FILENO
 *   Error  redirection  (2>) : cmd->err_file → STDERR_FILENO
 *
 * Called in the child process; a failure causes the child to exit(1) so
 * the parent detects a non-zero exit status.
 *
 * Parameters:
 *   cmd  – pointer to the Command whose redirection fields are inspected
 *
 * Returns:
 *    0  on success (all requested redirections applied)
 *   -1  on any failure (error already printed to stderr)
 * ----------------------------------------------------------------------------- */
int apply_redirections(const Command *cmd)
{
    /* ------------------------------------------------------------------
     * Input redirection:  command < file
     *
     * Open the file read-only.  If it does not exist, report the
     * spec-required "File not found." message and abort.
     * ------------------------------------------------------------------ */
    if (cmd->in_file != NULL) {
        /* O_RDONLY: open for reading only; file must already exist */
        int fd = open(cmd->in_file, O_RDONLY);
        if (fd < 0) {
            /* The spec requires this exact phrasing for a missing input file */
            fprintf(stderr, "File not found.\n");
            return -1;
        }

        /* Replace the child's stdin with this file descriptor.
         * After dup2() the child will read from the file instead of the
         * terminal (or the previous pipe end). */
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("dup2: stdin redirection");
            close(fd);
            return -1;
        }

        /* Close the original fd; we only need STDIN_FILENO from now on */
        close(fd);
    }

    /* ------------------------------------------------------------------
     * Output redirection:  command > file
     *
     * Open (or create) the file for writing, truncating any existing
     * content – matching standard shell '>' behaviour.
     * ------------------------------------------------------------------ */
    if (cmd->out_file != NULL) {
        /* O_WRONLY  : write-only access
         * O_CREAT   : create the file if it does not exist
         * O_TRUNC   : truncate to zero length if it already exists
         * 0644      : rw-r--r-- permissions for newly created files     */
        int fd = open(cmd->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror(cmd->out_file);
            return -1;
        }

        /* Replace the child's stdout with this file descriptor.
         * Subsequent writes (printf, puts, write to fd 1) land in the file. */
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("dup2: stdout redirection");
            close(fd);
            return -1;
        }

        close(fd);
    }

    /* ------------------------------------------------------------------
     * Error redirection:  command 2> file
     *
     * Same open/truncate semantics as output redirection, but the target
     * descriptor is STDERR_FILENO (fd 2) instead of STDOUT_FILENO (fd 1).
     * ------------------------------------------------------------------ */
    if (cmd->err_file != NULL) {
        int fd = open(cmd->err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror(cmd->err_file);
            return -1;
        }

        /* Replace the child's stderr; future fprintf(stderr, ...) calls
         * write to this file rather than the terminal. */
        if (dup2(fd, STDERR_FILENO) < 0) {
            perror("dup2: stderr redirection");
            close(fd);
            return -1;
        }

        close(fd);
    }

    /* All requested redirections succeeded */
    return 0;
}
