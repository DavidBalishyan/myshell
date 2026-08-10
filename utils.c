#include "shell.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) { perror("lsh: malloc"); exit(2); }
  return p;
}

void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) { perror("lsh: realloc"); exit(2); }
  return q;
}

char *xstrdup(const char *s) {
  char *p = strdup(s ? s : "");
  if (!p) { perror("lsh: strdup"); exit(2); }
  return p;
}

char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n); p[n] = '\0';
  return p;
}

char *read_all(FILE *stream) {
  size_t len = 0, cap = 4096;
  char *buf = xmalloc(cap);
  for (;;) {
    if (len + 2048 + 1 > cap) { cap *= 2; buf = xrealloc(buf, cap); }
    size_t n = fread(buf + len, 1, cap - len - 1, stream);
    len += n;
    if (n == 0) {
      if (ferror(stream) && errno == EINTR) { clearerr(stream); continue; }
      break;
    }
  }
  buf[len] = '\0';
  return buf;
}

bool valid_name(const char *s, size_t n) {
  if (!n || !((s[0] >= 'A' && s[0] <= 'Z') ||
              (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return false;
  for (size_t i = 1; i < n; i++)
    if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
          (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
  return true;
}

char *quote_remove(const char *word, bool *was_quoted) {
  size_t n = strlen(word), j = 0;
  char *out = xmalloc(n + 1);
  bool sq = false, dq = false, quoted = false;
  for (size_t i = 0; i < n; i++) {
    char c = word[i];
    if (sq) { if (c == '\'') { sq = false; quoted = true; } else out[j++] = c; continue; }
    if (dq) {
      if (c == '"') { dq = false; quoted = true; continue; }
      if (c == '\\' && i + 1 < n && strchr("$`\\\"\n", word[i + 1])) {
        quoted = true; if (word[i + 1] != '\n') out[j++] = word[++i]; else i++; continue;
      }
      out[j++] = c; continue;
    }
    if (c == '\'') { sq = true; quoted = true; }
    else if (c == '"') { dq = true; quoted = true; }
    else if (c == '\\' && i + 1 < n) { quoted = true; out[j++] = word[++i]; }
    else out[j++] = c;
  }
  out[j] = '\0';
  if (was_quoted) *was_quoted = quoted;
  return out;
}

char *ast_command_name(Ast *ast) {
  if (!ast) return xstrdup("");
  if (ast->type == AST_SIMPLE && ast->u.simple.nwords)
    return quote_remove(ast->u.simple.words[0], NULL);
  return xstrdup("compound command");
}
