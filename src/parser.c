// src/parser.c
#include <stdlib.h>     // malloc, free, NULL
#include <ctype.h>    // isspace, isdigit
#include <string.h>   // memcpy, strlen
#include <stdio.h>    // snprintf
#include "../include/parser.h" 

// ================ Parsing memory cleanup ================

// Function for freeing all memory allocated inside a Pipeline structure by the parse_line() function. 
void free_pipeline(Pipeline *p) {
    if (p == NULL) return;

    // If cmds is NULL, still reset fields and return.
    if (p->cmds == NULL) {
        p->n_cmds = 0;
        return;
    }

    for (int i = 0; i < p->n_cmds; i++) {
        Command *c = &p->cmds[i];

        // Free argv strings, then argv array
        if (c->argv != NULL) {
            for (int j = 0; c->argv[j] != NULL; j++) {
                free(c->argv[j]);
            }
            free(c->argv);
        }

        // Free redirection file strings
        free(c->in_file);
        free(c->out_file);
        free(c->err_file);

        // Optional: reset pointers to avoid accidental reuse
        c->argv = NULL;
        c->in_file = NULL;
        c->out_file = NULL;
        c->err_file = NULL;
    }

    free(p->cmds);
    p->cmds = NULL;
    p->n_cmds = 0;
}

// Helper function to initialize a Pipeline structure to an empty state.
static void pipeline_init(Pipeline *p) {
    if (p == NULL) return;
    p->cmds = NULL;
    p->n_cmds = 0;
}

// ================ tokenizer that recognizes operators and words ================
static void free_tokens(char **tokens, int ntok) {
    if (!tokens) return;
    for (int i = 0; i < ntok; i++) {
        free(tokens[i]);
    }
    free(tokens);
}

static int push_token(char ***tokens, int *ntok, int *cap,
                      const char *start, int len) {
    if (len <= 0) return 0;

    if (*ntok >= *cap) {
        int newcap = (*cap == 0) ? 8 : (*cap * 2);
        char **tmp = realloc(*tokens, (size_t)newcap * sizeof(char*));
        if (!tmp) return -1;
        *tokens = tmp;
        *cap = newcap;
    }

    char *s = malloc((size_t)len + 1);
    if (!s) return -1;
    memcpy(s, start, (size_t)len);
    s[len] = '\0';

    (*tokens)[(*ntok)++] = s;
    return 0;
}

static int tokenize(const char *line, char ***tokens_out, int *ntok_out,
                    char *err, size_t err_sz) {
    *tokens_out = NULL;
    *ntok_out = 0;
    if (err && err_sz > 0) err[0] = '\0';

    if (!line) return 0;

    char **tokens = NULL;
    int ntok = 0;
    int cap = 0;

    const char *p = line;

    while (*p) {
        // 1) Skip whitespace
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        // 2) Recognize operator: 2>
        if (*p == '2' && *(p + 1) == '>') {
            if (push_token(&tokens, &ntok, &cap, p, 2) != 0) goto oom;
            p += 2;
            continue;
        }

        // 3) Recognize single-char operators: < > |
        if (*p == '<' || *p == '>' || *p == '|') {
            if (push_token(&tokens, &ntok, &cap, p, 1) != 0) goto oom;
            p += 1;
            continue;
        }

        // 4) Otherwise: read a "word" token until whitespace or operator
        const char *start = p;
        while (*p &&
               !isspace((unsigned char)*p) &&
               *p != '<' && *p != '>' && *p != '|' ) {
            // stop at "2>" if we see it starting
            if (*p == '2' && *(p + 1) == '>') break;
            p++;
        }

        if (push_token(&tokens, &ntok, &cap, start, (int)(p - start)) != 0) goto oom;
    }

    *tokens_out = tokens;
    *ntok_out = ntok;
    return 0;

oom:
    if (err && err_sz > 0) {
        snprintf(err, err_sz, "Out of memory.");
    }
    free_tokens(tokens, ntok);
    *tokens_out = NULL;
    *ntok_out = 0;
    return 1;
}