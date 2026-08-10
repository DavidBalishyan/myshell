#include "shell.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  TokenList *tokens;
  size_t at;
  char *error;
  bool incomplete;
} Parser;

static Token *peek(Parser *p) { return &p->tokens->v[p->at]; }
static bool take(Parser *p, TokenType t) {
  if (peek(p)->type != t) return false;
  p->at++; return true;
}
static bool word_is(Parser *p, const char *s) {
  return peek(p)->type == TOK_WORD && !strcmp(peek(p)->text, s);
}
static bool take_word(Parser *p, const char *s) {
  if (!word_is(p, s)) return false;
  p->at++; return true;
}
static void fail(Parser *p, bool incomplete, const char *fmt, ...) {
  if (p->error) return;
  va_list ap; va_start(ap, fmt);
  char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  p->error = xstrdup(buf); p->incomplete = incomplete;
}
static Ast *node(AstType type) {
  Ast *n = xmalloc(sizeof(*n)); memset(n, 0, sizeof(*n)); n->type = type; return n;
}
static void skip_newlines(Parser *p) { while (take(p, TOK_NEWLINE)) {} }

static bool is_redir(TokenType t) {
  return t == TOK_LESS || t == TOK_GREAT || t == TOK_DGREAT || t == TOK_DLESS ||
         t == TOK_LESSAND || t == TOK_GREATAND || t == TOK_LESSGREAT;
}

static Redir *parse_redir(Parser *p) {
  int fd = -1;
  if (peek(p)->type == TOK_IO_NUMBER) {
    fd = atoi(peek(p)->text); p->at++;
    if (!is_redir(peek(p)->type)) { p->at--; return NULL; }
  }
  TokenType op = peek(p)->type;
  if (!is_redir(op)) return NULL;
  p->at++;
  if (peek(p)->type != TOK_WORD && peek(p)->type != TOK_IO_NUMBER) {
    fail(p, peek(p)->type == TOK_EOF, "line %zu: redirection requires a word", peek(p)->line);
    return NULL;
  }
  Token *operand = peek(p); p->at++;
  Redir *r = xmalloc(sizeof(*r)); memset(r, 0, sizeof(*r));
  r->fd = fd >= 0 ? fd : (op == TOK_LESS || op == TOK_DLESS || op == TOK_LESSAND || op == TOK_LESSGREAT ? 0 : 1);
  r->word = xstrdup(operand->text);
  switch (op) {
    case TOK_LESS: r->type = REDIR_IN; break;
    case TOK_GREAT: r->type = REDIR_OUT; break;
    case TOK_DGREAT: r->type = REDIR_APPEND; break;
    case TOK_DLESS: {
      r->type = REDIR_HEREDOC;
      bool quoted = false; char *d = quote_remove(operand->text, &quoted); free(d);
      r->heredoc_expand = !quoted; r->heredoc = xstrdup(operand->heredoc ? operand->heredoc : "");
      break;
    }
    case TOK_LESSAND: r->type = REDIR_DUP_IN; break;
    case TOK_GREATAND: r->type = REDIR_DUP_OUT; break;
    case TOK_LESSGREAT: r->type = REDIR_INOUT; break;
    default: break;
  }
  return r;
}

static void append_redir(Redir **head, Redir *r) {
  if (!r) return;
  while (*head) head = &(*head)->next;
  *head = r;
}

static Ast *parse_list(Parser *p, const char **stops, size_t nstops);
static Ast *parse_command(Parser *p);

static bool at_stop(Parser *p, const char **stops, size_t nstops) {
  if (peek(p)->type == TOK_EOF || peek(p)->type == TOK_RPAREN || peek(p)->type == TOK_DSEMI) return true;
  for (size_t i = 0; i < nstops; i++) if (word_is(p, stops[i])) return true;
  return false;
}

static Ast *parse_if_tail(Parser *p, bool already_consumed) {
  if (!already_consumed && !take_word(p, "if")) return NULL;
  const char *then_stop[] = {"then"};
  Ast *condition = parse_list(p, then_stop, 1);
  if (!condition || !take_word(p, "then")) {
    if (!p->error) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected 'then'", peek(p)->line);
    return NULL;
  }
  skip_newlines(p);
  const char *body_stops[] = {"elif", "else", "fi"};
  Ast *then_part = parse_list(p, body_stops, 3);
  Ast *else_part = NULL;
  if (take_word(p, "elif")) else_part = parse_if_tail(p, true);
  else if (take_word(p, "else")) {
    skip_newlines(p);
    const char *fi_stop[] = {"fi"};
    else_part = parse_list(p, fi_stop, 1);
    if (!take_word(p, "fi")) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected 'fi'", peek(p)->line);
  } else if (!take_word(p, "fi")) {
    fail(p, peek(p)->type == TOK_EOF, "line %zu: expected 'fi'", peek(p)->line);
  }
  Ast *n = node(AST_IF);
  n->u.if_clause.condition = condition;
  n->u.if_clause.then_part = then_part;
  n->u.if_clause.else_part = else_part;
  return n;
}

static Ast *parse_loop(Parser *p, AstType type) {
  p->at++;
  const char *do_stop[] = {"do"};
  Ast *condition = parse_list(p, do_stop, 1);
  if (!condition || !take_word(p, "do")) {
    if (!p->error) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected 'do'", peek(p)->line);
    return NULL;
  }
  skip_newlines(p);
  const char *done_stop[] = {"done"};
  Ast *body = parse_list(p, done_stop, 1);
  if (!take_word(p, "done")) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected 'done'", peek(p)->line);
  Ast *n = node(type); n->u.loop.condition = condition; n->u.loop.body = body; return n;
}

static Ast *parse_for(Parser *p) {
  p->at++;
  if (peek(p)->type != TOK_WORD || !valid_name(peek(p)->text, strlen(peek(p)->text))) {
    fail(p, peek(p)->type == TOK_EOF, "line %zu: expected a variable name after 'for'", peek(p)->line); return NULL;
  }
  Ast *n = node(AST_FOR); n->u.for_clause.name = xstrdup(peek(p)->text); p->at++;
  if (take_word(p, "in")) {
    while (peek(p)->type == TOK_WORD || peek(p)->type == TOK_IO_NUMBER) {
      n->u.for_clause.words = xrealloc(n->u.for_clause.words, (n->u.for_clause.nwords + 1) * sizeof(char *));
      n->u.for_clause.words[n->u.for_clause.nwords++] = xstrdup(peek(p)->text); p->at++;
    }
  }
  if (!take(p, TOK_SEMI) && !take(p, TOK_NEWLINE)) {
    fail(p, peek(p)->type == TOK_EOF, "line %zu: expected separator before 'do'", peek(p)->line); return n;
  }
  skip_newlines(p);
  if (!take_word(p, "do")) { fail(p, peek(p)->type == TOK_EOF, "line %zu: expected 'do'", peek(p)->line); return n; }
  skip_newlines(p);
  const char *done_stop[] = {"done"};
  n->u.for_clause.body = parse_list(p, done_stop, 1);
  if (!take_word(p, "done")) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected 'done'", peek(p)->line);
  return n;
}

static Ast *parse_case(Parser *p) {
  p->at++;
  if (peek(p)->type != TOK_WORD) { fail(p, peek(p)->type == TOK_EOF, "line %zu: expected word after 'case'", peek(p)->line); return NULL; }
  Ast *n = node(AST_CASE); n->u.case_clause.word = xstrdup(peek(p)->text); p->at++;
  skip_newlines(p);
  if (!take_word(p, "in")) { fail(p, peek(p)->type == TOK_EOF, "line %zu: expected 'in'", peek(p)->line); return n; }
  skip_newlines(p);
  CaseArm **tail = &n->u.case_clause.arms;
  while (!word_is(p, "esac") && peek(p)->type != TOK_EOF) {
    take(p, TOK_LPAREN); /* optional */
    CaseArm *arm = xmalloc(sizeof(*arm)); memset(arm, 0, sizeof(*arm));
    do {
      if (peek(p)->type != TOK_WORD && peek(p)->type != TOK_IO_NUMBER) {
        fail(p, false, "line %zu: expected case pattern", peek(p)->line); free(arm); return n;
      }
      arm->patterns = xrealloc(arm->patterns, (arm->npatterns + 1) * sizeof(char *));
      arm->patterns[arm->npatterns++] = xstrdup(peek(p)->text); p->at++;
    } while (take(p, TOK_PIPE));
    if (!take(p, TOK_RPAREN)) { fail(p, peek(p)->type == TOK_EOF, "line %zu: expected ')'", peek(p)->line); free(arm); return n; }
    skip_newlines(p);
    const char *esac_stop[] = {"esac"};
    arm->body = parse_list(p, esac_stop, 1);
    if (take(p, TOK_DSEMI)) skip_newlines(p);
    *tail = arm; tail = &arm->next;
  }
  if (!take_word(p, "esac")) fail(p, true, "line %zu: expected 'esac'", peek(p)->line);
  return n;
}

static Ast *parse_compound(Parser *p) {
  Ast *n = NULL;
  if (take(p, TOK_LPAREN)) {
    skip_newlines(p); n = node(AST_SUBSHELL); n->u.unary.body = parse_list(p, NULL, 0);
    if (!take(p, TOK_RPAREN)) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected ')'", peek(p)->line);
  } else if (take_word(p, "{")) {
    skip_newlines(p); const char *stop[] = {"}"};
    n = node(AST_GROUP); n->u.unary.body = parse_list(p, stop, 1);
    if (!take_word(p, "}")) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected '}'", peek(p)->line);
  } else if (word_is(p, "if")) n = parse_if_tail(p, false);
  else if (word_is(p, "while")) n = parse_loop(p, AST_WHILE);
  else if (word_is(p, "until")) n = parse_loop(p, AST_UNTIL);
  else if (word_is(p, "for")) n = parse_for(p);
  else if (word_is(p, "case")) n = parse_case(p);
  return n;
}

static Ast *parse_simple(Parser *p) {
  Ast *n = node(AST_SIMPLE);
  while (!p->error) {
    size_t save = p->at;
    Redir *r = parse_redir(p);
    if (r) { append_redir(&n->redirs, r); continue; }
    p->at = save;
    if (peek(p)->type != TOK_WORD && peek(p)->type != TOK_IO_NUMBER) break;
    n->u.simple.words = xrealloc(n->u.simple.words, (n->u.simple.nwords + 1) * sizeof(char *));
    n->u.simple.words[n->u.simple.nwords++] = xstrdup(peek(p)->text); p->at++;
  }
  if (!n->u.simple.nwords && !n->redirs) { ast_free(n); return NULL; }
  return n;
}

static Ast *parse_command(Parser *p) {
  /* name() compound-command */
  if (p->at + 2 < p->tokens->n && peek(p)->type == TOK_WORD && p->tokens->v[p->at + 1].type == TOK_LPAREN &&
      p->tokens->v[p->at + 2].type == TOK_RPAREN && valid_name(peek(p)->text, strlen(peek(p)->text))) {
    char *name = xstrdup(peek(p)->text); p->at += 3; skip_newlines(p);
    Ast *body = parse_compound(p);
    if (!body) { fail(p, peek(p)->type == TOK_EOF, "line %zu: function body must be a compound command", peek(p)->line); free(name); return NULL; }
    Ast *fn = node(AST_FUNCTION); fn->u.function.name = name; fn->u.function.body = body;
    while (!p->error) { Redir *r = parse_redir(p); if (!r) break; append_redir(&fn->redirs, r); }
    return fn;
  }
  Ast *n = parse_compound(p);
  if (!n) n = parse_simple(p);
  if (n && n->type != AST_SIMPLE) {
    while (!p->error) { Redir *r = parse_redir(p); if (!r) break; append_redir(&n->redirs, r); }
  }
  return n;
}

static Ast *parse_pipeline(Parser *p) {
  bool negate = take_word(p, "!");
  Ast *first = parse_command(p);
  if (!first) { if (!p->error) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected command", peek(p)->line); return NULL; }
  if (!take(p, TOK_PIPE)) {
    if (negate) { Ast *n = node(AST_NOT); n->u.unary.body = first; return n; }
    return first;
  }
  Ast *pipe = node(AST_PIPELINE);
  pipe->u.pipeline.commands = xrealloc(NULL, sizeof(Ast *));
  pipe->u.pipeline.commands[pipe->u.pipeline.ncommands++] = first;
  do {
    skip_newlines(p); Ast *part = parse_command(p);
    if (!part) { if (!p->error) fail(p, peek(p)->type == TOK_EOF, "line %zu: expected command after '|'", peek(p)->line); return pipe; }
    pipe->u.pipeline.commands = xrealloc(pipe->u.pipeline.commands, (pipe->u.pipeline.ncommands + 1) * sizeof(Ast *));
    pipe->u.pipeline.commands[pipe->u.pipeline.ncommands++] = part;
  } while (take(p, TOK_PIPE));
  if (negate) { Ast *n = node(AST_NOT); n->u.unary.body = pipe; return n; }
  return pipe;
}

static Ast *parse_and_or(Parser *p) {
  Ast *left = parse_pipeline(p);
  while (left && (peek(p)->type == TOK_AND_IF || peek(p)->type == TOK_OR_IF)) {
    TokenType op = peek(p)->type; p->at++; skip_newlines(p);
    Ast *right = parse_pipeline(p);
    if (!right) return left;
    Ast *n = node(op == TOK_AND_IF ? AST_AND : AST_OR);
    n->u.binary.left = left; n->u.binary.right = right; left = n;
  }
  return left;
}

static Ast *sequence(Ast *left, Ast *right) {
  if (!left) return right;
  if (!right) return left;
  Ast *n = node(AST_SEQUENCE); n->u.binary.left = left; n->u.binary.right = right; return n;
}

static Ast *parse_list(Parser *p, const char **stops, size_t nstops) {
  skip_newlines(p);
  Ast *root = NULL;
  while (!p->error && !at_stop(p, stops, nstops)) {
    Ast *part = parse_and_or(p);
    if (!part) break;
    bool separated = false;
    if (take(p, TOK_AMP)) {
      Ast *bg = node(AST_BACKGROUND); bg->u.unary.body = part; part = bg;
      separated = true;
    } else if (take(p, TOK_SEMI)) separated = true;
    root = sequence(root, part);
    if (peek(p)->type == TOK_NEWLINE) skip_newlines(p);
    else if (separated) continue;
    else if (!at_stop(p, stops, nstops) && peek(p)->type != TOK_EOF) {
      fail(p, false, "line %zu: unexpected token '%s'", peek(p)->line, peek(p)->text); break;
    }
  }
  return root;
}

Ast *parse_tokens(TokenList *tokens, char **error, bool *incomplete) {
  Parser p = {.tokens = tokens, .incomplete = tokens->incomplete};
  Ast *root = parse_list(&p, NULL, 0);
  if (!p.error && peek(&p)->type != TOK_EOF)
    fail(&p, false, "line %zu: unexpected token '%s'", peek(&p)->line, peek(&p)->text);
  if (tokens->error && !p.error) p.error = xstrdup(tokens->error);
  if (error) *error = p.error; else free(p.error);
  if (incomplete) *incomplete = p.incomplete;
  if (p.error) { ast_free(root); return NULL; }
  return root;
}

static void redirs_free(Redir *r) {
  while (r) { Redir *next = r->next; free(r->word); free(r->heredoc); free(r); r = next; }
}

void ast_free(Ast *n) {
  if (!n) return;
  redirs_free(n->redirs);
  switch (n->type) {
    case AST_SIMPLE:
      for (size_t i = 0; i < n->u.simple.nwords; i++) free(n->u.simple.words[i]);
      free(n->u.simple.words); break;
    case AST_SEQUENCE: case AST_AND: case AST_OR:
      ast_free(n->u.binary.left); ast_free(n->u.binary.right); break;
    case AST_PIPELINE:
      for (size_t i = 0; i < n->u.pipeline.ncommands; i++) ast_free(n->u.pipeline.commands[i]);
      free(n->u.pipeline.commands); break;
    case AST_SUBSHELL: case AST_GROUP: case AST_BACKGROUND: case AST_NOT:
      ast_free(n->u.unary.body); break;
    case AST_IF:
      ast_free(n->u.if_clause.condition); ast_free(n->u.if_clause.then_part); ast_free(n->u.if_clause.else_part); break;
    case AST_WHILE: case AST_UNTIL:
      ast_free(n->u.loop.condition); ast_free(n->u.loop.body); break;
    case AST_FOR:
      free(n->u.for_clause.name); for (size_t i = 0; i < n->u.for_clause.nwords; i++) free(n->u.for_clause.words[i]);
      free(n->u.for_clause.words); ast_free(n->u.for_clause.body); break;
    case AST_CASE: {
      free(n->u.case_clause.word); CaseArm *a = n->u.case_clause.arms;
      while (a) { CaseArm *next = a->next; for (size_t i = 0; i < a->npatterns; i++) free(a->patterns[i]); free(a->patterns); ast_free(a->body); free(a); a = next; }
      break;
    }
    case AST_FUNCTION: free(n->u.function.name); ast_free(n->u.function.body); break;
  }
  free(n);
}
