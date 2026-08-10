#include "shell.h"

#include <stdlib.h>
#include <string.h>

static bool redir_op(TokenType t) {
  return t == TOK_LESS || t == TOK_GREAT || t == TOK_DGREAT || t == TOK_DLESS ||
         t == TOK_LESSAND || t == TOK_GREATAND || t == TOK_LESSGREAT;
}

static bool assignment_word(const char *s) {
  const char *eq = strchr(s, '='); return eq && valid_name(s, (size_t)(eq - s));
}

static bool reserved_word(const char *s) {
  static const char *const words[] = {"!","{","}","case","do","done","elif","else","esac","fi","for","if","in","then","until","while"};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) if (!strcmp(s, words[i])) return true;
  return false;
}

static void replace_token(TokenList *list, size_t at, TokenList *replacement) {
  size_t rn = replacement->n ? replacement->n - 1 : 0;
  free(list->v[at].text); free(list->v[at].heredoc);
  if (rn > 1) list->v = xrealloc(list->v, (list->n + rn - 1) * sizeof(Token));
  if (rn != 1) memmove(list->v + at + rn, list->v + at + 1, (list->n - at - 1) * sizeof(Token));
  for (size_t i = 0; i < rn; i++) list->v[at + i] = replacement->v[i];
  list->n = list->n + rn - 1;
  free(replacement->v[replacement->n - 1].text); free(replacement->v[replacement->n - 1].heredoc);
  free(replacement->v); free(replacement->error); memset(replacement, 0, sizeof(*replacement));
}

void alias_expand_tokens(Shell *sh, TokenList *tokens) {
  bool command_position = true, redir_operand = false; unsigned expansions = 0;
  for (size_t i = 0; i + 1 < tokens->n;) {
    Token *t = &tokens->v[i];
    if (redir_operand) { redir_operand = false; i++; continue; }
    if (redir_op(t->type)) { redir_operand = true; i++; continue; }
    if (t->type == TOK_IO_NUMBER && i + 1 < tokens->n && redir_op(tokens->v[i + 1].type)) { i++; continue; }
    if (t->type == TOK_SEMI || t->type == TOK_AMP || t->type == TOK_PIPE || t->type == TOK_AND_IF ||
        t->type == TOK_OR_IF || t->type == TOK_NEWLINE || t->type == TOK_LPAREN) { command_position = true; i++; continue; }
    if (t->type == TOK_RPAREN || t->type == TOK_DSEMI) { command_position = false; i++; continue; }
    if (t->type != TOK_WORD) { i++; continue; }
    if (reserved_word(t->text)) { command_position = true; i++; continue; }
    if (command_position && assignment_word(t->text)) { i++; continue; }
    if (command_position && expansions++ < 64) {
      const char *value = shell_getalias(sh, t->text);
      if (value) {
        TokenList r = lex_source(value);
        if (!r.error && !r.incomplete) { replace_token(tokens, i, &r); continue; }
        token_list_free(&r);
      }
    }
    command_position = false; i++;
  }
}
