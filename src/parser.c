// src/parser.c
#include <stdlib.h>     // malloc, free, NULL
#include <ctype.h>    // isspace, isdigit
#include <string.h>   // memcpy, strlen
#include <stdio.h>    // snprintf
#include "parser.h"

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

        // Reset pointers to avoid accidental reuse
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

// ================ Tokenizer that recognizes operators and words ================

// Helper function to free an array of tokens.
static void free_tokens(char **tokens, int ntok) {
    if (!tokens) return;
    for (int i = 0; i < ntok; i++) {
        free(tokens[i]);
    }
    free(tokens);
}

// Helper function to push a new token into the tokens array, resizing if necessary.
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

// Tokenize the input line into an array of tokens, recognizing operators and words.
// Rules:
// 1) Split on whitespace
// 2) Recognize operator "2>" as a single token
// 3) Treat <, >, | as separate tokens even without spaces

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

// Helper function to check if a token is an operator.

static int is_op(const char *t) {
    return (strcmp(t, "<") == 0 ||
            strcmp(t, ">") == 0 ||
            strcmp(t, "2>") == 0 ||
            strcmp(t, "|") == 0);
}

// Build argv array from tokens[start..end-1], skipping redirection operators + filenames.
// Returns 0 on success, nonzero on OOM.
// On success: *argv_out is NULL-terminated.
static int build_argv(char **tokens, int start, int end, char ***argv_out) {
    *argv_out = NULL;

    // First count how many argv words we will include
    int count = 0;
    for (int i = start; i < end; i++) {
        if (strcmp(tokens[i], "<") == 0 || strcmp(tokens[i], ">") == 0 || strcmp(tokens[i], "2>") == 0) {
            i++; // skip the filename token (if it exists; syntax checked elsewhere)
            continue;
        }
        if (strcmp(tokens[i], "|") == 0) continue; // pipes are not part of argv
        count++;
    }

    // Allocate argv[count+1] for NULL terminator
    char **argv = (char**)calloc((size_t)count + 1, sizeof(char*));
    if (!argv) return 1;

    int k = 0;
    for (int i = start; i < end; i++) {
        if (strcmp(tokens[i], "<") == 0 || strcmp(tokens[i], ">") == 0 || strcmp(tokens[i], "2>") == 0) {
            i++; // skip filename
            continue;
        }
        if (strcmp(tokens[i], "|") == 0) continue;

        argv[k] = strdup(tokens[i]);
        if (!argv[k]) {
            // cleanup partial
            for (int j = 0; j < k; j++) free(argv[j]);
            free(argv);
            return 1;
        }
        k++;
    }
    argv[k] = NULL;

    *argv_out = argv;
    return 0;
}

// ================ Main parse_line function ================

int parse_line(const char *line, Pipeline *out, char *err, size_t err_sz) {
    pipeline_init(out);
    if (err && err_sz > 0) err[0] = '\0';

    char **tokens = NULL;
    int ntok = 0;

    if (tokenize(line, &tokens, &ntok, err, err_sz) != 0) {
        // tokenizer already filled err
        goto fail;
    }

    // Blank line => do nothing, but not an error
    if (ntok == 0) {
        free_tokens(tokens, ntok);
        return 1; // main should just reprompt when err is empty
    }

    // ----------------------------
    // A) Pipe syntax validation
    // ----------------------------
    // Cannot start with '|'
    if (strcmp(tokens[0], "|") == 0) {
        if (err && err_sz > 0) snprintf(err, err_sz, "Command missing after pipe.");
        goto fail;
    }
    // Cannot end with '|'
    if (strcmp(tokens[ntok - 1], "|") == 0) {
        if (err && err_sz > 0) snprintf(err, err_sz, "Command missing after pipe.");
        goto fail;
    }
    // Cannot have '| |' (with nothing between)
    for (int i = 0; i < ntok - 1; i++) {
        if (strcmp(tokens[i], "|") == 0 && strcmp(tokens[i + 1], "|") == 0) {
            if (err && err_sz > 0) snprintf(err, err_sz, "Empty command between pipes.");
            goto fail;
        }
    }

    // Count commands = number of pipes + 1
    int n_cmds = 1;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "|") == 0) n_cmds++;
    }

    out->cmds = (Command*)calloc((size_t)n_cmds, sizeof(Command));
    if (!out->cmds) {
        if (err && err_sz > 0) snprintf(err, err_sz, "Out of memory.");
        goto fail;
    }
    out->n_cmds = n_cmds;

    // ----------------------------
    // B) Parse each command segment
    // ----------------------------
    int cmd_index = 0;
    int seg_start = 0;

    for (int i = 0; i <= ntok; i++) {
        int is_end = (i == ntok) || (strcmp(tokens[i], "|") == 0);
        if (!is_end) continue;

        int seg_end = i; // tokens[seg_start .. seg_end-1] is this command segment

        // 1) Validate redirections in this segment and collect filenames
        Command *c = &out->cmds[cmd_index];

        for (int j = seg_start; j < seg_end; j++) {
            if (strcmp(tokens[j], "<") == 0) {
                if (j + 1 >= seg_end || is_op(tokens[j + 1])) {
                    if (err && err_sz > 0) snprintf(err, err_sz, "Input file not specified.");
                    goto fail;
                }
                // last one wins if multiple appear
                free(c->in_file);
                c->in_file = strdup(tokens[j + 1]);
                if (!c->in_file) { if (err && err_sz > 0) snprintf(err, err_sz, "Out of memory."); goto fail; }
                j++; // skip filename
            } else if (strcmp(tokens[j], ">") == 0) {
                if (j + 1 >= seg_end || is_op(tokens[j + 1])) {
                    // Special message when '>' appears at end of a later segment in pipeline
                    // Spec example: "< input.txt | command1 >" => "Output file not specified after redirection."
                    if (err && err_sz > 0) {
                        if (n_cmds > 1 && seg_end == ntok) snprintf(err, err_sz, "Output file not specified after redirection.");
                        else snprintf(err, err_sz, "Output file not specified.");
                    }
                    goto fail;
                }
                free(c->out_file);
                c->out_file = strdup(tokens[j + 1]);
                if (!c->out_file) { if (err && err_sz > 0) snprintf(err, err_sz, "Out of memory."); goto fail; }
                j++;
            } else if (strcmp(tokens[j], "2>") == 0) {
                if (j + 1 >= seg_end || is_op(tokens[j + 1])) {
                    if (err && err_sz > 0) snprintf(err, err_sz, "Error output file not specified.");
                    goto fail;
                }
                free(c->err_file);
                c->err_file = strdup(tokens[j + 1]);
                if (!c->err_file) { if (err && err_sz > 0) snprintf(err, err_sz, "Out of memory."); goto fail; }
                j++;
            }
        }

        // 2) Build argv for this segment (skip redirection tokens)
        if (build_argv(tokens, seg_start, seg_end, &c->argv) != 0) {
            if (err && err_sz > 0) snprintf(err, err_sz, "Out of memory.");
            goto fail;
        }

        // 3) Ensure there is at least one argv word (a command name)
        if (c->argv == NULL || c->argv[0] == NULL) {
            // This catches cases like: "< input.txt" with no command
            if (err && err_sz > 0) snprintf(err, err_sz, "Command missing after pipe.");
            goto fail;
        }

        cmd_index++;
        seg_start = i + 1; // next segment starts after '|'
    }

    free_tokens(tokens, ntok);
    return 0;

fail:
    free_tokens(tokens, ntok);
    free_pipeline(out);
    return 1;
}
