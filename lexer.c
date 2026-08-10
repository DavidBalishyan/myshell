#include "shell.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { Token *v; size_t n, cap; } TVec;
typedef struct { size_t *v; size_t n, cap; } IVec;

static void push(TVec *a, TokenType type, const char *s, size_t n, size_t line) {
  if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 64; a->v = xrealloc(a->v, a->cap * sizeof(*a->v)); }
  a->v[a->n++] = (Token){type, xstrndup(s ? s : "", n), NULL, line};
}

static void ipush(IVec *a, size_t n) {
  if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->v = xrealloc(a->v, a->cap * sizeof(*a->v)); }
  a->v[a->n++] = n;
}

static bool all_digits(const char *s, size_t n) {
  if (!n) return false;
  for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return false;
  return true;
}

/* Skip a ${...}, $(...), or $((...)) unit while retaining its spelling. */
static bool skip_dollar_unit(const char *s, size_t *at, bool *incomplete) {
  size_t i = *at;
  if (s[i] != '$' || (s[i + 1] != '(' && s[i + 1] != '{')) return false;
  char open = s[i + 1], close = open == '(' ? ')' : '}';
  int depth = 1;
  bool sq = false, dq = false, bt = false;
  i += 2;
  if (open == '(' && s[i] == '(') { depth = 2; i++; }
  for (; s[i]; i++) {
    char c = s[i];
    if (c == '\\' && !sq && s[i + 1]) { i++; continue; }
    if (bt) { if (c == '`') bt = false; continue; }
    if (sq) { if (c == '\'') sq = false; continue; }
    if (dq) { if (c == '"') dq = false; else if (c == '`') bt = true; continue; }
    if (c == '\'') { sq = true; continue; }
    if (c == '"') { dq = true; continue; }
    if (c == '`') { bt = true; continue; }
    if (c == open) depth++;
    else if (c == close && --depth == 0) { *at = i + 1; return true; }
  }
  *incomplete = true;
  *at = i;
  return true;
}

static size_t scan_word(const char *s, size_t i, bool *incomplete) {
  bool sq = false, dq = false, bt = false;
  while (s[i]) {
    char c = s[i];
    if (sq) { if (c == '\'') sq = false; i++; continue; }
    if (bt) {
      if (c == '\\' && s[i + 1]) i += 2;
      else { if (c == '`') bt = false; i++; }
      continue;
    }
    if (dq) {
      if (c == '"') { dq = false; i++; continue; }
      if (c == '\\' && s[i + 1]) { i += 2; continue; }
      if (c == '`') { bt = true; i++; continue; }
      if (c == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) {
        skip_dollar_unit(s, &i, incomplete); continue;
      }
      i++; continue;
    }
    if (c == '\'') { sq = true; i++; continue; }
    if (c == '"') { dq = true; i++; continue; }
    if (c == '`') { bt = true; i++; continue; }
    if (c == '\\') { if (s[i + 1]) i += 2; else { *incomplete = true; i++; } continue; }
    if (c == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) {
      skip_dollar_unit(s, &i, incomplete); continue;
    }
    if (isspace((unsigned char)c) || strchr(";&|<>()", c)) break;
    i++;
  }
  if (sq || dq || bt) *incomplete = true;
  return i;
}

static bool delimiter_matches(const char *source, size_t start, size_t end, const char *delimiter) {
  if (end > start && source[end - 1] == '\r') end--;
  return strlen(delimiter) == end - start && !memcmp(source + start, delimiter, end - start);
}

static void collect_heredocs(const char *source, size_t *at, size_t *line,
                             TVec *tokens, IVec *pending, TokenList *result) {
  for (size_t p = 0; p < pending->n; p++) {
    Token *delimiter_token = &tokens->v[pending->v[p]];
    char *delimiter = quote_remove(delimiter_token->text, NULL);
    size_t body_start = *at;
    bool found = false;
    while (source[*at]) {
      size_t line_start = *at;
      while (source[*at] && source[*at] != '\n') (*at)++;
      size_t line_end = *at;
      if (delimiter_matches(source, line_start, line_end, delimiter)) {
        delimiter_token->heredoc = xstrndup(source + body_start, line_start - body_start);
        if (source[*at] == '\n') { (*at)++; (*line)++; }
        found = true;
        break;
      }
      if (source[*at] == '\n') { (*at)++; (*line)++; }
    }
    if (!found) {
      result->incomplete = true;
      delimiter_token->heredoc = xstrndup(source + body_start, *at - body_start);
    }
    free(delimiter);
  }
  pending->n = 0;
}

TokenList lex_source(const char *source) {
  TokenList result = {0};
  TVec tokens = {0};
  IVec pending = {0};
  size_t i = 0, line = 1;

  while (source[i]) {
    if (source[i] == ' ' || source[i] == '\t' || source[i] == '\r') { i++; continue; }
    if (source[i] == '\\' && source[i + 1] == '\n') { i += 2; line++; continue; }
    if (source[i] == '#') {
      while (source[i] && source[i] != '\n') i++;
      continue;
    }
    if (source[i] == '\n') {
      push(&tokens, TOK_NEWLINE, "\n", 1, line++); i++;
      if (pending.n) collect_heredocs(source, &i, &line, &tokens, &pending, &result);
      continue;
    }

    TokenType type = TOK_EOF;
    size_t oplen = 0;
    struct Op { const char *s; TokenType t; } ops[] = {
      {"&&", TOK_AND_IF}, {"||", TOK_OR_IF}, {">>", TOK_DGREAT},
      {"<<", TOK_DLESS}, {"<&", TOK_LESSAND}, {">&", TOK_GREATAND},
      {"<>", TOK_LESSGREAT}, {";;", TOK_DSEMI}, {";", TOK_SEMI},
      {"&", TOK_AMP}, {"|", TOK_PIPE}, {"<", TOK_LESS}, {">", TOK_GREAT},
      {"(", TOK_LPAREN}, {")", TOK_RPAREN}
    };
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
      size_t n = strlen(ops[k].s);
      if (!strncmp(source + i, ops[k].s, n)) { type = ops[k].t; oplen = n; break; }
    }
    if (oplen) {
      push(&tokens, type, source + i, oplen, line); i += oplen;
      continue;
    }

    size_t start = i;
    i = scan_word(source, i, &result.incomplete);
    if (i == start) { result.error = xstrdup("lexer made no progress"); break; }
    TokenType wt = all_digits(source + start, i - start) &&
                   (source[i] == '<' || source[i] == '>') ? TOK_IO_NUMBER : TOK_WORD;
    push(&tokens, wt, source + start, i - start, line);
    /* A delimiter is the word immediately after the most recent << token. */
    if (tokens.n >= 2 && tokens.v[tokens.n - 2].type == TOK_DLESS)
      ipush(&pending, tokens.n - 1);
  }
  if (pending.n) collect_heredocs(source, &i, &line, &tokens, &pending, &result);
  push(&tokens, TOK_EOF, "", 0, line);
  free(pending.v);
  result.v = tokens.v; result.n = tokens.n;
  return result;
}

void token_list_free(TokenList *tokens) {
  if (!tokens) return;
  for (size_t i = 0; i < tokens->n; i++) {
    free(tokens->v[i].text); free(tokens->v[i].heredoc);
  }
  free(tokens->v); free(tokens->error);
  memset(tokens, 0, sizeof(*tokens));
}
