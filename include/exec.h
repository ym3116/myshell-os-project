#ifndef EXEC_H
#define EXEC_H

#include "parser.h"

int execute_pipeline(const Pipeline *p);


int apply_redirections(const Command *cmd);


int create_pipes(int n_pipes, int (*pipe_fds)[2]);


void close_all_pipes(int n_pipes, int (*pipe_fds)[2]);


void connect_pipes_for_child(int cmd_idx, int n_cmds,
                             int n_pipes, int (*pipe_fds)[2]);

#endif /* EXEC_H */
