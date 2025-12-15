#ifndef SHELL_H
#define SHELL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Function Declarations for built-in shell commands:
int lsh_cd(char **args);
int lsh_help(char **args);
int lsh_exit(char **args);

// Function Declarations for shell logic:
void lsh_loop(void);
char *lsh_read_line(void);
char **lsh_split_line(char *line);
int lsh_launch(char **args);
int lsh_execute(char **args);

#endif
