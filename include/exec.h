#ifndef EXEC_H
#define EXEC_H

#include "parser.h"

/**
 * Execute a validated pipeline.
 *
 * Returns:
 *  0 on success
 *  nonzero on runtime error
 *
 * Runtime errors include:
 *  - input file open failure => "File not found."
 *  - execvp failure => "Command not found." or
 *    "Command not found in pipe sequence." :contentReference[oaicite:3]{index=3}
 *
 * Waiting behavior:
 *  - must wait for the last process in the pipeline before prompting again :contentReference[oaicite:4]{index=4}
 */
int execute_pipeline(const Pipeline *p);

#endif