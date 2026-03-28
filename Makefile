CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude
LDFLAGS =

SRC     = src/main.c src/exec.c src/parser.c src/pipe.c src/redir.c
OBJ     = $(SRC:.c=.o)
BIN     = myshell

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean