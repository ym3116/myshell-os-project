// src/main.c
// src/main.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "exec.h"

int main(void) {
    char *line = NULL;
    size_t cap = 0;

    while (1) {
        // Prompt
        printf("$ ");
        fflush(stdout);

        // Read line (EOF/Ctrl-D => exit)
        ssize_t nread = getline(&line, &cap, stdin);
        if (nread < 0) {
            printf("\n");
            break;
        }

        // Strip trailing newline
        if (nread > 0 && line[nread - 1] == '\n') {
            line[nread - 1] = '\0';
        }

        // Ignore empty/whitespace-only lines
        int only_ws = 1;
        for (char *p = line; *p; p++) {
            if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                only_ws = 0;
                break;
            }
        }
        if (only_ws) continue;

        // Built-in: exit
        if (strcmp(line, "exit") == 0) {
            break;
        }

        // Parse
        Pipeline pl;
        char errbuf[256];

        int rc = parse_line(line, &pl, errbuf, sizeof(errbuf));
        if (rc != 0) {
            // Print syntax/validation error if provided
            if (errbuf[0] != '\0') {
                fprintf(stderr, "%s\n", errbuf);
            }
            free_pipeline(&pl);
            continue;
        }

        // Execute (not implemented yet, will be implemented by Utidi)
        (void)execute_pipeline(&pl);

        // Cleanup
        free_pipeline(&pl);
    }

    free(line);
    return 0;
}