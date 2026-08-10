#include "shell.h"

#include <ctype.h>
#include <glob.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XF_SPLIT 1u
#define XF_QUOTED 2u

typedef struct {
  char *s;
  unsigned char *flags;
  size_t n, cap;
  bool keep_empty;
} XField;
typedef struct { XField *v; size_t n, cap; } XVec;

static void xf_init(XField *f) { memset(f, 0, sizeof(*f)); }
static void xf_putn(XField *f, const char *s, size_t n, unsigned char flags) {
  if (f->n + n + 1 > f->cap) {
    size_t cap = f->cap ? f->cap : 32; while (cap < f->n + n + 1) cap *= 2;
    f->s = xrealloc(f->s, cap); f->flags = xrealloc(f->flags, cap); f->cap = cap;
  }
  memcpy(f->s + f->n, s, n); memset(f->flags + f->n, flags, n); f->n += n; f->s[f->n] = '\0';
}
static void xf_puts(XField *f, const char *s, unsigned char flags) { xf_putn(f, s, strlen(s), flags); }
static void xv_push(XVec *v, XField f) {
  if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 4; v->v = xrealloc(v->v, v->cap * sizeof(*v->v)); }
  v->v[v->n++] = f;
}
static XField *xv_last(XVec *v) { return &v->v[v->n - 1]; }
static void xv_free(XVec *v) {
  for (size_t i = 0; i < v->n; i++) { free(v->v[i].s); free(v->v[i].flags); }
  free(v->v); memset(v, 0, sizeof(*v));
}

static char *special_value(Shell *sh, const char *name, size_t n, bool *set) {
  char buf[64]; *set = true;
  if (n == 1 && name[0] == '?') { snprintf(buf, sizeof(buf), "%d", sh->last_status); return xstrdup(buf); }
  if (n == 1 && name[0] == '$') { snprintf(buf, sizeof(buf), "%ld", (long)sh->shell_pid); return xstrdup(buf); }
  if (n == 1 && name[0] == '!') {
    if (!sh->last_bg) { *set = false; return xstrdup(""); }
    snprintf(buf, sizeof(buf), "%ld", (long)sh->last_bg); return xstrdup(buf);
  }
  if (n == 1 && name[0] == '#') { snprintf(buf, sizeof(buf), "%zu", sh->npositional); return xstrdup(buf); }
  if (n == 1 && name[0] == '0') return xstrdup(sh->name);
  if (n == 1 && name[0] == '-') {
    size_t j = 0; if (sh->opt_errexit) buf[j++] = 'e'; if (sh->opt_noglob) buf[j++] = 'f';
    if (sh->opt_nounset) buf[j++] = 'u';
    if (sh->opt_xtrace) buf[j++] = 'x';
    buf[j] = '\0'; return xstrdup(buf);
  }
  bool digits = n > 0;
  for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)name[i])) digits = false;
  if (digits) {
    char *number = xstrndup(name, n), *end; unsigned long k = strtoul(number, &end, 10); free(number);
    (void)end;
    if (k == 0) return xstrdup(sh->name);
    if (k <= sh->npositional) return xstrdup(sh->positional[k - 1]);
    *set = false; return xstrdup("");
  }
  char *key = xstrndup(name, n); const char *value = shell_getvar(sh, key, set); char *out = xstrdup(value); free(key); return out;
}

static char *join_positional(Shell *sh, char separator) {
  size_t n = 1;
  for (size_t i = 0; i < sh->npositional; i++) n += strlen(sh->positional[i]) + (i != 0);
  char *s = xmalloc(n), *at = s;
  for (size_t i = 0; i < sh->npositional; i++) {
    if (i && separator) *at++ = separator;
    size_t z = strlen(sh->positional[i]); memcpy(at, sh->positional[i], z); at += z;
  }
  *at = '\0'; return s;
}

static size_t match_unit(const char *s, size_t i, char open, char close) {
  int depth = 1; bool sq = false, dq = false;
  for (; s[i]; i++) {
    char c = s[i];
    if (c == '\\' && !sq && s[i + 1]) { i++; continue; }
    if (sq) { if (c == '\'') sq = false; continue; }
    if (dq) { if (c == '"') dq = false; continue; }
    if (c == '\'') sq = true; else if (c == '"') dq = true;
    else if (c == open) depth++; else if (c == close && --depth == 0) return i;
  }
  return i;
}

static void expansion_error(Shell *sh, const char *name, const char *message) {
  fprintf(stderr, "%s: %s: %s\n", sh->name, name, *message ? message : "parameter null or not set");
  sh->last_status = 2; sh->expansion_failed = true;
}

static char *parameter_expand(Shell *sh, const char *inside, size_t n, bool *set_out) {
  if (n > 1 && inside[0] == '#' && (valid_name(inside + 1, n - 1) ||
      (n == 2 && (strchr("?$!#@*-", inside[1]) || isdigit((unsigned char)inside[1]))))) {
    bool inner_set; char *inner = special_value(sh, inside + 1, n - 1, &inner_set);
    char length[64]; snprintf(length, sizeof(length), "%zu", strlen(inner)); free(inner); *set_out = inner_set; return xstrdup(length);
  }
  size_t name_n = 0;
  if (n && strchr("?$!#@*-", inside[0])) name_n = 1;
  else if (n && isdigit((unsigned char)inside[0])) while (name_n < n && isdigit((unsigned char)inside[name_n])) name_n++;
  else while (name_n < n && (isalnum((unsigned char)inside[name_n]) || inside[name_n] == '_')) name_n++;
  bool colon = name_n < n && inside[name_n] == ':';
  size_t oi = name_n + (colon ? 1 : 0);
  char op = oi < n && strchr("-=+?", inside[oi]) ? inside[oi] : '\0';
  size_t word_i = op ? oi + 1 : name_n;
  bool set = false; char *value;
  if (name_n == 1 && (inside[0] == '@' || inside[0] == '*')) {
    bool ifs_set; const char *ifs = shell_getvar(sh, "IFS", &ifs_set); char sep = !ifs_set ? ' ' : (*ifs ? *ifs : '\0');
    value = join_positional(sh, sep); set = sh->npositional != 0;
  } else value = special_value(sh, inside, name_n, &set);
  bool null = !*value;
  bool use_word = !set || (colon && null);
  if (!op) { *set_out = set; return value; }
  char *word = xstrndup(inside + word_i, n - word_i);
  char *expanded = expand_string(sh, word); free(word);
  if (op == '-' && use_word) { free(value); value = expanded; expanded = NULL; set = true; }
  else if (op == '=' && use_word) {
    if (valid_name(inside, name_n)) { char *name = xstrndup(inside, name_n); shell_setvar(sh, name, expanded, false); free(name); }
    free(value); value = expanded; expanded = NULL; set = true;
  } else if (op == '+' && !use_word) { free(value); value = expanded; expanded = NULL; }
  else if (op == '+' && use_word) { free(value); value = xstrdup(""); }
  else if (op == '?' && use_word) { char *name = xstrndup(inside, name_n); expansion_error(sh, name, expanded); free(name); free(value); value = xstrdup(""); }
  free(expanded); *set_out = set; return value;
}

static void append_at_quoted(Shell *sh, XVec *out) {
  if (!sh->npositional) { xv_last(out)->keep_empty = false; return; }
  xf_puts(xv_last(out), sh->positional[0], XF_QUOTED); xv_last(out)->keep_empty = true;
  for (size_t i = 1; i < sh->npositional; i++) {
    XField f; xf_init(&f); xf_puts(&f, sh->positional[i], XF_QUOTED); f.keep_empty = true; xv_push(out, f);
  }
}

static void expand_raw(Shell *sh, const char *word, XVec *out) {
  bool sq = false, dq = false;
  XField first; xf_init(&first); xv_push(out, first);
  size_t n = strlen(word);
  for (size_t i = 0; i < n;) {
    char c = word[i];
    if (sq) {
      if (c == '\'') { sq = false; i++; xv_last(out)->keep_empty = true; }
      else { xf_putn(xv_last(out), word + i, 1, XF_QUOTED); i++; }
      continue;
    }
    if (c == '\'' && !dq) { sq = true; xv_last(out)->keep_empty = true; i++; continue; }
    if (c == '"') { dq = !dq; xv_last(out)->keep_empty = true; i++; continue; }
    if (c == '\\') {
      if (i + 1 < n && (!dq || strchr("$`\\\"\n", word[i + 1]))) {
        if (word[i + 1] != '\n') xf_putn(xv_last(out), word + i + 1, 1, dq ? XF_QUOTED : XF_QUOTED);
        i += 2; continue;
      }
      xf_putn(xv_last(out), word + i++, 1, dq ? XF_QUOTED : 0); continue;
    }
    if (c == '~' && !dq && i == 0 && (word[i + 1] == '/' || word[i + 1] == '\0')) {
      bool hs; const char *home = shell_getvar(sh, "HOME", &hs); if (hs) xf_puts(xv_last(out), home, 0); else xf_putn(xv_last(out), "~", 1, 0);
      i++; continue;
    }
    if (c == '`') {
      size_t end = i + 1; while (end < n && word[end] != '`') { if (word[end] == '\\' && end + 1 < n) end++; end++; }
      char *cmd = xstrndup(word + i + 1, end - i - 1), *value = NULL;
      command_substitute(sh, cmd, &value); xf_puts(xv_last(out), value, dq ? XF_QUOTED : XF_SPLIT);
      free(cmd); free(value); i = end < n ? end + 1 : end; continue;
    }
    if (c != '$') { xf_putn(xv_last(out), word + i++, 1, dq ? XF_QUOTED : 0); continue; }
    if (i + 1 >= n) { xf_putn(xv_last(out), "$", 1, dq ? XF_QUOTED : 0); i++; continue; }
    if (word[i + 1] == '(') {
      if (i + 2 < n && word[i + 2] == '(') {
        size_t end = match_unit(word, i + 3, '(', ')');
        size_t expr_end = end > 0 && word[end + 1] == ')' ? end : end;
        char *expr = xstrndup(word + i + 3, expr_end - (i + 3)); bool ok;
        long value = arithmetic_eval(sh, expr, &ok); char buf[64]; snprintf(buf, sizeof(buf), "%ld", value);
        xf_puts(xv_last(out), buf, dq ? XF_QUOTED : XF_SPLIT); free(expr);
        i = word[end + 1] == ')' ? end + 2 : end + 1; continue;
      }
      size_t end = match_unit(word, i + 2, '(', ')');
      char *cmd = xstrndup(word + i + 2, end - i - 2), *value = NULL;
      command_substitute(sh, cmd, &value); xf_puts(xv_last(out), value, dq ? XF_QUOTED : XF_SPLIT);
      free(cmd); free(value); i = end + (word[end] ? 1 : 0); continue;
    }
    if (word[i + 1] == '{') {
      size_t end = match_unit(word, i + 2, '{', '}'); bool set;
      if (dq && end == i + 3 && word[i + 2] == '@') { append_at_quoted(sh, out); i = end + (word[end] ? 1 : 0); continue; }
      char *value = parameter_expand(sh, word + i + 2, end - i - 2, &set);
      xf_puts(xv_last(out), value, dq ? XF_QUOTED : XF_SPLIT); if (dq) xv_last(out)->keep_empty = true;
      free(value); i = end + (word[end] ? 1 : 0); continue;
    }
    size_t start = i + 1, z = 0;
    if (strchr("?$!#@*-", word[start]) || isdigit((unsigned char)word[start])) z = 1;
    else while (start + z < n && (isalnum((unsigned char)word[start + z]) || word[start + z] == '_')) z++;
    if (!z) { xf_putn(xv_last(out), "$", 1, dq ? XF_QUOTED : 0); i++; continue; }
    if (z == 1 && word[start] == '@' && dq) { append_at_quoted(sh, out); i = start + z; continue; }
    bool set; char *value;
    if (z == 1 && (word[start] == '@' || word[start] == '*')) {
      bool ifs_set; const char *ifs = shell_getvar(sh, "IFS", &ifs_set); value = join_positional(sh, !ifs_set ? ' ' : (*ifs ? *ifs : '\0')); set = sh->npositional != 0;
    } else value = special_value(sh, word + start, z, &set);
    if (!set && sh->opt_nounset) { char *key = xstrndup(word + start, z); expansion_error(sh, key, "parameter not set"); free(key); }
    xf_puts(xv_last(out), value, dq ? XF_QUOTED : XF_SPLIT); if (dq) xv_last(out)->keep_empty = true;
    free(value); i = start + z;
  }
}

static bool ifs_char(const char *ifs, char c) { return strchr(ifs, c) != NULL; }
static bool ifs_white(char c) { return c == ' ' || c == '\t' || c == '\n'; }

static XVec split_fields(Shell *sh, XVec *raw, bool do_split) {
  XVec result = {0};
  bool is_set; const char *ifs = shell_getvar(sh, "IFS", &is_set); if (!is_set) ifs = " \t\n";
  for (size_t k = 0; k < raw->n; k++) {
    XField *src = &raw->v[k]; XField cur; xf_init(&cur); cur.keep_empty = src->keep_empty;
    bool emitted = false;
    for (size_t i = 0; i < src->n;) {
      bool delim = do_split && (src->flags[i] & XF_SPLIT) && ifs_char(ifs, src->s[i]);
      if (!delim) { xf_putn(&cur, src->s + i, 1, src->flags[i]); i++; continue; }
      char d = src->s[i++];
      if (cur.n || (!ifs_white(d) && emitted)) { xv_push(&result, cur); xf_init(&cur); emitted = true; }
      else if (!ifs_white(d)) { XField empty; xf_init(&empty); empty.keep_empty = true; xv_push(&result, empty); emitted = true; }
      if (ifs_white(d)) while (i < src->n && (src->flags[i] & XF_SPLIT) && ifs_char(ifs, src->s[i]) && ifs_white(src->s[i])) i++;
    }
    if (cur.n || cur.keep_empty) xv_push(&result, cur); else { free(cur.s); free(cur.flags); }
  }
  return result;
}

static void final_push(Fields *f, const char *s) { f->v = xrealloc(f->v, (f->n + 1) * sizeof(char *)); f->v[f->n++] = xstrdup(s); }

static Fields pathname_expand(Shell *sh, XVec *fields, bool enabled) {
  Fields out = {0};
  for (size_t k = 0; k < fields->n; k++) {
    XField *f = &fields->v[k]; bool magic = false; size_t cap = f->n * 2 + 2, j = 0; char *pattern = xmalloc(cap);
    for (size_t i = 0; i < f->n; i++) {
      bool q = f->flags[i] & XF_QUOTED;
      if (!q && strchr("*?[", f->s[i])) magic = true;
      if (q && strchr("*?[\\", f->s[i])) pattern[j++] = '\\';
      pattern[j++] = f->s[i];
    }
    pattern[j] = '\0';
    if (enabled && magic && !sh->opt_noglob) {
      glob_t g; memset(&g, 0, sizeof(g)); int rc = glob(pattern, 0, NULL, &g);
      if (rc == 0) for (size_t i = 0; i < g.gl_pathc; i++) final_push(&out, g.gl_pathv[i]);
      else final_push(&out, f->s ? f->s : "");
      globfree(&g);
    } else final_push(&out, f->s ? f->s : "");
    free(pattern);
  }
  return out;
}

Fields expand_word(Shell *sh, const char *word, bool split_glob, bool assignment) {
  (void)assignment;
  /* This exact special case avoids manufacturing an empty argument for "$@". */
  if ((!strcmp(word, "\"$@\"") || !strcmp(word, "\"${@}\"")) && sh->npositional == 0) return (Fields){0};
  XVec raw = {0}; expand_raw(sh, word, &raw);
  XVec split = split_fields(sh, &raw, split_glob); xv_free(&raw);
  Fields result = pathname_expand(sh, &split, split_glob); xv_free(&split); return result;
}

char *expand_string(Shell *sh, const char *word) {
  Fields f = expand_word(sh, word, false, false);
  if (!f.n) return xstrdup("");
  char *s = xstrdup(f.v[0]); fields_free(&f); return s;
}

char *expand_heredoc(Shell *sh, const char *body) {
  /* Quotes have no syntactic role in here-document bodies. Escape them before
     using the normal no-splitting expansion path. Backslashes retain their
     POSIX double-quote-like treatment there. */
  size_t n = strlen(body), j = 0; char *raw = xmalloc(n * 2 + 1);
  for (size_t i = 0; i < n; i++) {
    if (body[i] == '\'' || body[i] == '"') raw[j++] = '\\';
    raw[j++] = body[i];
  }
  raw[j] = '\0'; char *out = expand_string(sh, raw); free(raw); return out;
}

char *expand_case_pattern(Shell *sh, const char *word) {
  XVec raw = {0}; expand_raw(sh, word, &raw);
  if (!raw.n) return xstrdup("");
  XField *f = &raw.v[0]; size_t cap = f->n * 2 + 1, j = 0; char *pattern = xmalloc(cap);
  for (size_t i = 0; i < f->n; i++) {
    if ((f->flags[i] & XF_QUOTED) && strchr("*?[\\", f->s[i])) pattern[j++] = '\\';
    pattern[j++] = f->s[i];
  }
  pattern[j] = '\0'; xv_free(&raw); return pattern;
}

void fields_free(Fields *f) {
  for (size_t i = 0; i < f->n; i++) free(f->v[i]);
  free(f->v); f->v = NULL; f->n = 0;
}

/* Recursive-descent POSIX integer arithmetic. */
typedef struct { Shell *sh; const char *s; bool ok; } AP;
static void askip(AP *p) { while (isspace((unsigned char)*p->s)) p->s++; }
static long aexpr(AP *p);
static long aprimary(AP *p) {
  askip(p);
  if (*p->s == '(') { p->s++; long v = aexpr(p); askip(p); if (*p->s == ')') p->s++; else p->ok = false; return v; }
  if (isalpha((unsigned char)*p->s) || *p->s == '_') {
    const char *b = p->s++; while (isalnum((unsigned char)*p->s) || *p->s == '_') p->s++;
    char *n = xstrndup(b, p->s - b); bool set; const char *s = shell_getvar(p->sh, n, &set); free(n);
    return set ? strtol(s, NULL, 0) : 0;
  }
  char *end; long v = strtol(p->s, &end, 0); if (end == p->s) { p->ok = false; return 0; } p->s = end; return v;
}
static long aunary(AP *p) { askip(p); if (*p->s == '+') { p->s++; return aunary(p); } if (*p->s == '-') { p->s++; return -aunary(p); } if (*p->s == '!') { p->s++; return !aunary(p); } if (*p->s == '~') { p->s++; return ~aunary(p); } return aprimary(p); }
#define BIN(name,next,op1,op2,body) static long name(AP*p){long v=next(p);for(;;){askip(p);if(op2&&p->s[0]==op1&&p->s[1]==op2){p->s+=2;long r=next(p);body;}else if(!op2&&p->s[0]==op1){p->s++;long r=next(p);body;}else break;}return v;}
BIN(amul,aunary,'*',0,v*=r)
static long amul2(AP*p){long v=amul(p);for(;;){askip(p);if(*p->s=='/'){p->s++;long r=aunary(p);if(!r){p->ok=false;r=1;}v/=r;}else if(*p->s=='%'){p->s++;long r=aunary(p);if(!r){p->ok=false;r=1;}v%=r;}else break;}return v;}
static long aadd(AP*p){long v=amul2(p);for(;;){askip(p);if(*p->s=='+'){p->s++;v+=amul2(p);}else if(*p->s=='-'){p->s++;v-=amul2(p);}else break;}return v;}
static long ashift(AP*p){long v=aadd(p);for(;;){askip(p);if(!strncmp(p->s,"<<",2)){p->s+=2;v<<=aadd(p);}else if(!strncmp(p->s,">>",2)){p->s+=2;v>>=aadd(p);}else break;}return v;}
static long arel(AP*p){long v=ashift(p);for(;;){askip(p);if(!strncmp(p->s,"<=",2)){p->s+=2;v=v<=ashift(p);}else if(!strncmp(p->s,">=",2)){p->s+=2;v=v>=ashift(p);}else if(*p->s=='<'&&p->s[1]!='<'){p->s++;v=v<ashift(p);}else if(*p->s=='>'&&p->s[1]!='>'){p->s++;v=v>ashift(p);}else break;}return v;}
static long aeq(AP*p){long v=arel(p);for(;;){askip(p);if(!strncmp(p->s,"==",2)){p->s+=2;v=v==arel(p);}else if(!strncmp(p->s,"!=",2)){p->s+=2;v=v!=arel(p);}else break;}return v;}
static long aand(AP*p){long v=aeq(p);for(;;){askip(p);if(p->s[0]=='&'&&p->s[1]!='&'){p->s++;v&=aeq(p);}else break;}return v;}
BIN(axor,aand,'^',0,v^=r)
static long aor(AP*p){long v=axor(p);for(;;){askip(p);if(p->s[0]=='|'&&p->s[1]!='|'){p->s++;v|=axor(p);}else break;}return v;}
BIN(aland,aor,'&','&',v=(v&&r))
BIN(alor,aland,'|','|',v=(v||r))
static long aexpr(AP *p) {
  long condition = alor(p); askip(p);
  if (*p->s != '?') return condition;
  p->s++; long yes = aexpr(p); askip(p);
  if (*p->s != ':') { p->ok = false; return 0; }
  p->s++; long no = aexpr(p); return condition ? yes : no;
}
long arithmetic_eval(Shell *sh, const char *expression, bool *ok) {
  const char *begin = expression; while (isspace((unsigned char)*begin)) begin++;
  const char *name_end = begin;
  if (isalpha((unsigned char)*name_end) || *name_end == '_') {
    name_end++; while (isalnum((unsigned char)*name_end) || *name_end == '_') name_end++;
    const char *op = name_end; while (isspace((unsigned char)*op)) op++;
    const char *rhs = NULL; char operation = 0;
    if (*op == '=' && op[1] != '=') { operation = '='; rhs = op + 1; }
    else if (strchr("+-*/%&|^", *op) && op[1] == '=') { operation = *op; rhs = op + 2; }
    if (rhs) {
      bool rhs_ok; long value = arithmetic_eval(sh, rhs, &rhs_ok); char *name = xstrndup(begin, (size_t)(name_end - begin));
      if (rhs_ok && operation != '=') { bool set; long old = strtol(shell_getvar(sh, name, &set), NULL, 0); if (!set) old = 0;
        if (operation == '+') value = old + value; else if (operation == '-') value = old - value; else if (operation == '*') value = old * value;
        else if (operation == '/') { if (!value) rhs_ok = false; else value = old / value; } else if (operation == '%') { if (!value) rhs_ok = false; else value = old % value; }
        else if (operation == '&') value = old & value; else if (operation == '|') value = old | value; else if (operation == '^') value = old ^ value;
      }
      if (rhs_ok) { char value_s[64]; snprintf(value_s, sizeof(value_s), "%ld", value); shell_setvar(sh, name, value_s, false); }
      else { sh->expansion_failed = true; sh->last_status = 2; }
      free(name); if (ok) *ok = rhs_ok; return value;
    }
  }
  AP p = {sh, expression, true}; long v = aexpr(&p); askip(&p); if (*p.s) p.ok = false;
  if (!p.ok) { fprintf(stderr, "%s: arithmetic expression: syntax error: %s\n", sh->name, expression); sh->expansion_failed = true; sh->last_status = 2; }
  if (ok) *ok = p.ok;
  return v;
}
