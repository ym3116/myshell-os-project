#ifndef PARSER_H
#define PARSER_H

#include <stddef.h> // size_t

// One command segment in a pipeline: e.g.,  grep hello 2> err.log
typedef struct {
    char **argv;        // NULL-terminated, suitable for execvp()
    char  *in_file;     // for '<'  (NULL if none)
    char  *out_file;    // for '>'  (NULL if none)
    char  *err_file;    // for '2>' (NULL if none)
} Command;

// Full pipeline: cmd0 | cmd1 | cmd2 ...
typedef struct {
    Command *cmds;      // a pointer to a dynamically allocated array of Command structs
    int      n_cmds;    // the number of commands in the pipeline
} Pipeline;

/**
 * Parse and validate a command line.
 *
 * Returns:
 *  0 on success and fills `out`
 *  nonzero on syntax/validation error and writes message to `err`
 *
 * Parser should catch "shell syntax" issues required by spec:
 *  - missing file after <, >, 2>
 *  - missing command after pipe
 *  - empty command between pipes
 *
 * Note: command existence is NOT checked here (execvp will decide at runtime).
 */
int parse_line(const char *line, Pipeline *out, char *err, size_t err_sz);

/**
 * Free all memory allocated inside `Pipeline` by parse_line().
 * Safe to call even if parsing failed partially.
 */
void free_pipeline(Pipeline *p);

#endif