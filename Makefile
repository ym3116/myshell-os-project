CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude
LDFLAGS = -lpthread

# ---------------------------------------------------------------------------
# Shared object files: the Phase 1 shell logic (parser, executor, pipes,
# redirections) compiled once and linked into BOTH server and client binaries.
# ---------------------------------------------------------------------------
SHARED_SRC = src/exec.c src/parser.c src/pipe.c src/redir.c
SHARED_OBJ = $(SHARED_SRC:.c=.o)

# ---------------------------------------------------------------------------
# Three binaries:
#   myshell        - original Phase 1 standalone shell (kept for reference)
#   myshell_server - Phase 2 server: accepts connections and runs commands
#   myshell_client - Phase 2 client: sends commands and displays output
# ---------------------------------------------------------------------------
all: myshell myshell_server myshell_client

# Phase 1 standalone shell (main.c + shared logic)
myshell: src/main.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Phase 2 server binary (server.c + shared logic)
myshell_server: src/server.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Phase 2 client binary (client.c only — no shell logic needed on the client)
myshell_client: src/client.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Generic rule: compile any src/*.c -> src/*.o
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o myshell myshell_server myshell_client

.PHONY: all clean
