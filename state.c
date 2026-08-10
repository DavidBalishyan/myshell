#include "shell.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void shell_init(Shell *sh, const char *name, bool interactive) {
  memset(sh, 0, sizeof(*sh));
  sh->name = xstrdup(name ? name : "lsh");
  sh->interactive = interactive;
  sh->shell_pid = getpid();
  sh->next_job = 1;
  sh->last_status = 0;
  sh->getopts_index = 1;
  sh->getopts_offset = 1;
}

static void free_vars(Variable *v) {
  while (v) { Variable *next = v->next; free(v->name); free(v->value); free(v); v = next; }
}

void shell_destroy(Shell *sh) {
  free_vars(sh->vars);
  Function *f = sh->functions;
  while (f) { Function *next = f->next; free(f->name); free(f); f = next; }
  Job *j = sh->jobs;
  while (j) { Job *next = j->next; free(j->command); free(j); j = next; }
  AstOwner *o = sh->asts;
  while (o) { AstOwner *next = o->next; ast_free(o->ast); free(o); o = next; }
  Alias *a = sh->aliases;
  while (a) { Alias *next = a->next; free(a->name); free(a->value); free(a); a = next; }
  for (size_t i = 0; i < sh->npositional; i++) free(sh->positional[i]);
  free(sh->positional); free(sh->name);
  free(sh->trap_exit); free(sh->trap_int); free(sh->trap_term); free(sh->trap_hup);
}

static Variable *find_var(Shell *sh, const char *name) {
  for (Variable *v = sh->vars; v; v = v->next) if (!strcmp(v->name, name)) return v;
  return NULL;
}

const char *shell_getvar(Shell *sh, const char *name, bool *is_set) {
  Variable *v = find_var(sh, name);
  if (v) { if (is_set) *is_set = true; return v->value; }
  const char *e = getenv(name);
  if (is_set) *is_set = e != NULL;
  return e ? e : "";
}

int shell_setvar(Shell *sh, const char *name, const char *value, bool exported) {
  if (!valid_name(name, strlen(name))) return 1;
  Variable *v = find_var(sh, name);
  if (v && v->readonly) { fprintf(stderr, "%s: %s: is read only\n", sh->name, name); return 1; }
  if (!v) {
    v = xmalloc(sizeof(*v));
    *v = (Variable){xstrdup(name), xstrdup(value), exported, false, sh->vars};
    sh->vars = v;
  } else {
    free(v->value); v->value = xstrdup(value); v->exported = v->exported || exported;
  }
  if (v->exported && setenv(name, value, 1) < 0) { perror("lsh: setenv"); return 1; }
  return 0;
}

int shell_unsetvar(Shell *sh, const char *name) {
  Variable **at = &sh->vars;
  while (*at) {
    Variable *v = *at;
    if (!strcmp(v->name, name)) {
      if (v->readonly) { fprintf(stderr, "%s: %s: is read only\n", sh->name, name); return 1; }
      *at = v->next; free(v->name); free(v->value); free(v); unsetenv(name); return 0;
    }
    at = &v->next;
  }
  unsetenv(name); return 0;
}

const char *shell_getalias(Shell *sh, const char *name) {
  for (Alias *a = sh->aliases; a; a = a->next) if (!strcmp(a->name, name)) return a->value;
  return NULL;
}

void shell_setalias(Shell *sh, const char *name, const char *value) {
  for (Alias *a = sh->aliases; a; a = a->next) if (!strcmp(a->name, name)) {
    free(a->value); a->value = xstrdup(value); return;
  }
  Alias *a = xmalloc(sizeof(*a)); *a = (Alias){xstrdup(name), xstrdup(value), sh->aliases}; sh->aliases = a;
}

int shell_unsetalias(Shell *sh, const char *name) {
  Alias **at = &sh->aliases;
  while (*at) { if (!strcmp((*at)->name, name)) { Alias *a = *at; *at = a->next; free(a->name); free(a->value); free(a); return 0; } at = &(*at)->next; }
  return 1;
}
