// src/exec.c
#include <stdio.h>      // perror, fprintf
#include <stdlib.h>     // exit
#include <unistd.h>     // fork, execvp, _exit
#include <sys/wait.h>   // waitpid

#include "exec.h"

int execute_pipeline(const Pipeline *p) {
    if (p == NULL || p->n_cmds <= 0) {
        return 0; // nothing to do
    }

    // Step 1 only: support exactly 1 command (no pipes yet)
    if (p->n_cmds != 1) {
        fprintf(stderr, "Pipelines not implemented yet.\n");
        return 1;
    }

    const Command *c = &p->cmds[0];

    // If parser is correct, argv[0] exists, but be defensive
    if (c->argv == NULL || c->argv[0] == NULL) {
        fprintf(stderr, "Command not found.\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        // Child: execute the command
        // apply redirections BEFORE exec
        if (apply_redirections(c) != 0) {
            _exit(1);
        }
        execvp(c->argv[0], c->argv);

        // If execvp returns, it failed
        fprintf(stderr, "Command not found.\n");
        _exit(127);
    }

    // Parent: wait for child to finish
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    // Optional: you can return child's exit code if you want
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 1;
}