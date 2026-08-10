#ifndef LSH_SHELL_H
#define LSH_SHELL_H

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

typedef enum {
  TOK_WORD, TOK_IO_NUMBER, TOK_NEWLINE, TOK_SEMI, TOK_AMP, TOK_PIPE,
  TOK_AND_IF, TOK_OR_IF, TOK_LESS, TOK_GREAT, TOK_DGREAT, TOK_DLESS,
  TOK_LESSAND, TOK_GREATAND, TOK_LESSGREAT, TOK_DSEMI, TOK_LPAREN,
  TOK_RPAREN, TOK_EOF
} TokenType;

typedef struct {
  TokenType type;
  char *text;                 /* raw, quote-preserving spelling for words */
  char *heredoc;              /* body, attached to a << delimiter word */
  size_t line;
} Token;

typedef struct {
  Token *v;
  size_t n;
  char *error;
  bool incomplete;
} TokenList;

typedef enum {
  REDIR_IN, REDIR_OUT, REDIR_APPEND, REDIR_HEREDOC, REDIR_DUP_IN,
  REDIR_DUP_OUT, REDIR_INOUT
} RedirType;

typedef struct Redir {
  int fd;
  RedirType type;
  char *word;
  char *heredoc;
  bool heredoc_expand;
  struct Redir *next;
} Redir;

typedef enum {
  AST_SIMPLE, AST_SEQUENCE, AST_AND, AST_OR, AST_BACKGROUND, AST_PIPELINE,
  AST_SUBSHELL, AST_GROUP, AST_IF, AST_WHILE, AST_UNTIL, AST_FOR, AST_CASE,
  AST_FUNCTION, AST_NOT
} AstType;

typedef struct Ast Ast;

typedef struct CaseArm {
  char **patterns;
  size_t npatterns;
  Ast *body;
  struct CaseArm *next;
} CaseArm;

struct Ast {
  AstType type;
  Redir *redirs;
  union {
    struct { char **words; size_t nwords; } simple;
    struct { Ast *left, *right; } binary;
    struct { Ast **commands; size_t ncommands; } pipeline;
    struct { Ast *body; } unary;
    struct { Ast *condition, *then_part, *else_part; } if_clause;
    struct { Ast *condition, *body; } loop;
    struct { char *name; char **words; size_t nwords; Ast *body; } for_clause;
    struct { char *word; CaseArm *arms; } case_clause;
    struct { char *name; Ast *body; } function;
  } u;
};

typedef struct Variable {
  char *name, *value;
  bool exported, readonly;
  struct Variable *next;
} Variable;

typedef struct Function {
  char *name;
  Ast *body;
  Redir *redirs;
  struct Function *next;
} Function;

typedef struct Job {
  int id;
  pid_t pid, pgid;
  char *command;
  int status;
  bool done;
  struct Job *next;
} Job;

typedef struct AstOwner {
  Ast *ast;
  struct AstOwner *next;
} AstOwner;

typedef struct Alias {
  char *name, *value;
  struct Alias *next;
} Alias;

typedef enum { FLOW_NONE, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN, FLOW_EXIT } Flow;

typedef struct Shell {
  Variable *vars;
  Function *functions;
  Job *jobs;
  AstOwner *asts;
  Alias *aliases;
  int next_job;
  int last_status;
  pid_t last_bg;
  pid_t shell_pid;
  char *name;
  char **positional;
  size_t npositional;
  bool interactive;
  bool opt_errexit, opt_nounset, opt_xtrace, opt_noglob;
  bool should_exit;
  bool expansion_failed;
  int exit_status;
  Flow flow;
  int flow_count;
  int function_depth, loop_depth;
  int getopts_index, getopts_offset;
  char *trap_exit, *trap_int, *trap_term, *trap_hup;
} Shell;

typedef struct { char **v; size_t n; } Fields;

/* lexer/parser */
TokenList lex_source(const char *source);
void token_list_free(TokenList *tokens);
Ast *parse_tokens(TokenList *tokens, char **error, bool *incomplete);
void ast_free(Ast *ast);

/* state and expansion */
void shell_init(Shell *sh, const char *name, bool interactive);
void shell_destroy(Shell *sh);
const char *shell_getvar(Shell *sh, const char *name, bool *is_set);
int shell_setvar(Shell *sh, const char *name, const char *value, bool exported);
int shell_unsetvar(Shell *sh, const char *name);
const char *shell_getalias(Shell *sh, const char *name);
void shell_setalias(Shell *sh, const char *name, const char *value);
int shell_unsetalias(Shell *sh, const char *name);
void alias_expand_tokens(Shell *sh, TokenList *tokens);
bool valid_name(const char *s, size_t n);
Fields expand_word(Shell *sh, const char *word, bool split_glob, bool assignment);
char *expand_string(Shell *sh, const char *word);
char *expand_heredoc(Shell *sh, const char *body);
char *expand_case_pattern(Shell *sh, const char *word);
void fields_free(Fields *fields);
long arithmetic_eval(Shell *sh, const char *expression, bool *ok);

/* execution */
int execute_ast(Shell *sh, Ast *ast, bool child);
int execute_source(Shell *sh, const char *source, const char *label);
int execute_file(Shell *sh, const char *path, char **argv, size_t argc, bool dot);
int command_substitute(Shell *sh, const char *source, char **output);
void reap_jobs(Shell *sh, bool notify);
void shell_configure_signal(int sig, bool trapped);

/* builtins */
typedef int (*BuiltinFn)(Shell *, int, char **);
BuiltinFn builtin_lookup(const char *name, bool *special);
int builtin_run(Shell *sh, BuiltinFn fn, int argc, char **argv);

/* utilities */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *read_all(FILE *stream);
char *quote_remove(const char *word, bool *was_quoted);
char *ast_command_name(Ast *ast);

#endif
