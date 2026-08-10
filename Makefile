CC=gcc
CFLAGS=-Wall

lsh: main.o builtins.o utils.o
	$(CC) $(CFLAGS) -o lsh main.o builtins.o utils.o

main.o: main.c shell.h
	$(CC) $(CFLAGS) -c main.c

builtins.o: builtins.c shell.h
	$(CC) $(CFLAGS) -c builtins.c

utils.o: utils.c shell.h
	$(CC) $(CFLAGS) -c utils.c

clean:
	rm -f *.o lsh
