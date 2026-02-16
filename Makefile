CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11 -Iinclude

C_SRC = $(shell find src -type f -name '*.c')
OBJ = $(C_SRC:.c=.o)

.PHONY: all clean test

all: jcpp

jcpp: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) jcpp

test: jcpp
	bash tests/run.sh