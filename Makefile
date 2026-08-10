CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g

OBJS = main.o lexer.o alias.o parser.o state.o expand.o exec.o builtins.o utils.o

lsh: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c shell.h
	$(CC) $(CFLAGS) -c $<

test: lsh
	./tests/run.sh

clean:
	rm -f $(OBJS) lsh

.PHONY: clean test
