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


int parse_line(const char *line, Pipeline *out, char *err, size_t err_sz);


void free_pipeline(Pipeline *p);

#endif