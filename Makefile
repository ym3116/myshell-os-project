CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude
LDFLAGS = -lpthread

# ---------------------------------------------------------------------------
# Shared object files: Phase 1 shell logic linked into server and standalone shell
# ---------------------------------------------------------------------------
SHARED_SRC = src/exec.c src/parser.c src/pipe.c src/redir.c
SHARED_OBJ = $(SHARED_SRC:.c=.o)

# ---------------------------------------------------------------------------
# Binaries:
#   myshell        - Phase 1 standalone shell
#   myshell_server - Phase 4 scheduler-backed server
#   myshell_client - Phase 2/3/4 client (unchanged)
#   demo           - Phase 4 demo program for testing the scheduler
# ---------------------------------------------------------------------------
all: myshell myshell_server myshell_client demo

# Phase 1 standalone shell
myshell: src/main.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Phase 4 server: server.c + scheduler.c + shared shell logic
myshell_server: src/server.o src/scheduler.o $(SHARED_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Client binary (no shell logic needed)
myshell_client: src/client.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Demo program for scheduler testing
demo: src/demo.o
	$(CC) $(CFLAGS) -o $@ $^

# Generic rule: compile any src/*.c -> src/*.o
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o myshell myshell_server myshell_client demo

.PHONY: all clean
