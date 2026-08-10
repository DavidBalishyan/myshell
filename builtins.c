#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>

static int bi_colon(Shell *sh, int argc, char **argv) { (void)sh; (void)argc; (void)argv; return 0; }
static int bi_false(Shell *sh, int argc, char **argv) { (void)sh; (void)argc; (void)argv; return 1; }

static int bi_cd(Shell *sh, int argc, char **argv) {
  const char *dir;
  if (argc < 2) { bool set; dir = shell_getvar(sh, "HOME", &set); if (!set) { fprintf(stderr, "%s: cd: HOME not set\n", sh->name); return 1; } }
  else if (!strcmp(argv[1], "-")) { bool set; dir = shell_getvar(sh, "OLDPWD", &set); if (!set) { fprintf(stderr, "%s: cd: OLDPWD not set\n", sh->name); return 1; } puts(dir); }
  else dir = argv[1];
  char *old = getcwd(NULL, 0);
  if (chdir(dir) < 0) { fprintf(stderr, "%s: cd: %s: %s\n", sh->name, dir, strerror(errno)); free(old); return 1; }
  char *now = getcwd(NULL, 0);
  if (old) shell_setvar(sh, "OLDPWD", old, true);
  if (now) shell_setvar(sh, "PWD", now, true);
  free(old); free(now); return 0;
}

static int bi_pwd(Shell *sh, int argc, char **argv) {
  (void)argc; (void)argv; char *p = getcwd(NULL, 0);
  if (!p) { fprintf(stderr, "%s: pwd: %s\n", sh->name, strerror(errno)); return 1; }
  puts(p); free(p); return 0;
}

static int numeric_status(Shell *sh, const char *s, bool *ok) {
  char *end; errno = 0; long n = strtol(s, &end, 10);
  *ok = *s && !*end && errno != ERANGE;
  if (!*ok) fprintf(stderr, "%s: exit: %s: Illegal number\n", sh->name, s);
  return (unsigned char)n;
}

static int bi_exit(Shell *sh, int argc, char **argv) {
  int status = sh->last_status; bool ok = true;
  if (argc > 1) status = numeric_status(sh, argv[1], &ok);
  if (!ok) status = 2;
  else if (argc > 2) { fprintf(stderr, "%s: exit: too many arguments\n", sh->name); if (!sh->interactive) { sh->should_exit=true; sh->exit_status=2; sh->flow=FLOW_EXIT; return 2; } return 1; }
  sh->should_exit = true; sh->exit_status = status; sh->flow = FLOW_EXIT; return status;
}

static Variable *find_local(Shell *sh, const char *name) {
  for (Variable *v = sh->vars; v; v = v->next) if (!strcmp(v->name, name)) return v;
  return NULL;
}

static int assignment_arg(Shell *sh, const char *arg, bool export_it, bool readonly) {
  const char *eq = strchr(arg, '='); size_t n = eq ? (size_t)(eq - arg) : strlen(arg);
  if (!valid_name(arg, n)) { fprintf(stderr, "%s: %.*s: bad variable name\n", sh->name, (int)n, arg); return 1; }
  char *name = xstrndup(arg, n); bool set; const char *old = shell_getvar(sh, name, &set);
  if (shell_setvar(sh, name, eq ? eq + 1 : old, export_it) != 0) { free(name); return 1; }
  Variable *v = find_local(sh, name); if (readonly && v) v->readonly = true;
  free(name); return 0;
}

static int bi_export(Shell *sh, int argc, char **argv) {
  if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-p"))) {
    for (Variable *v = sh->vars; v; v = v->next) if (v->exported) printf("export %s='%s'\n", v->name, v->value);
    return 0;
  }
  int rc = 0; for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-p")) continue; rc |= assignment_arg(sh, argv[i], true, false); } return rc;
}

static int bi_readonly(Shell *sh, int argc, char **argv) {
  if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-p"))) {
    for (Variable *v = sh->vars; v; v = v->next) if (v->readonly) printf("readonly %s='%s'\n", v->name, v->value);
    return 0;
  }
  int rc = 0; for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-p")) continue; rc |= assignment_arg(sh, argv[i], false, true); } return rc;
}

static int bi_unset(Shell *sh, int argc, char **argv) {
  bool functions = false; int i = 1;
  if (i < argc && (!strcmp(argv[i], "-f") || !strcmp(argv[i], "-v"))) functions = argv[i++][1] == 'f';
  int rc = 0;
  for (; i < argc; i++) {
    if (!valid_name(argv[i], strlen(argv[i]))) { fprintf(stderr, "%s: unset: %s: bad variable name\n", sh->name, argv[i]); rc = 1; continue; }
    if (functions) {
      Function **at = &sh->functions; while (*at && strcmp((*at)->name, argv[i])) at = &(*at)->next;
      if (*at) { Function *f = *at; *at = f->next; free(f->name); free(f); }
    } else rc |= shell_unsetvar(sh, argv[i]);
  }
  return rc;
}

static void replace_positional(Shell *sh, int argc, char **argv) {
  for (size_t i = 0; i < sh->npositional; i++) free(sh->positional[i]);
  free(sh->positional);
  sh->npositional = argc > 0 ? (size_t)argc : 0; sh->positional = sh->npositional ? xmalloc(sh->npositional * sizeof(char *)) : NULL;
  for (size_t i = 0; i < sh->npositional; i++) sh->positional[i] = xstrdup(argv[i]);
}

static int set_option(Shell *sh, char option, bool enabled) {
  switch (option) {
    case 'e': sh->opt_errexit = enabled; break; case 'u': sh->opt_nounset = enabled; break;
    case 'x': sh->opt_xtrace = enabled; break; case 'f': sh->opt_noglob = enabled; break;
    default: fprintf(stderr, "%s: set: Illegal option -%c\n", sh->name, option); return 2;
  }
  return 0;
}

static int bi_set(Shell *sh, int argc, char **argv) {
  if (argc == 1) { for (Variable *v = sh->vars; v; v = v->next) printf("%s='%s'\n", v->name, v->value); return 0; }
  int i = 1, rc = 0;
  while (i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]) {
    bool on = argv[i][0] == '-';
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (size_t k = 1; argv[i][k]; k++) rc |= set_option(sh, argv[i][k], on);
    i++;
  }
  if (i < argc || (argc > 1 && !strcmp(argv[argc - 1], "--"))) replace_positional(sh, argc - i, argv + i);
  return rc;
}

static int bi_shift(Shell *sh, int argc, char **argv) {
  long n = 1; if (argc > 1) { char *e; n = strtol(argv[1], &e, 10); if (!*argv[1] || *e || n < 0) return 2; }
  if ((size_t)n > sh->npositional) { fprintf(stderr, "%s: shift: can't shift that many\n", sh->name); return 2; }
  for (long i = 0; i < n; i++) free(sh->positional[i]);
  memmove(sh->positional, sh->positional + n, (sh->npositional - (size_t)n) * sizeof(char *)); sh->npositional -= (size_t)n; return 0;
}

static int flow_builtin(Shell *sh, int argc, char **argv, Flow flow) {
  int n = 1; if (argc > 1) { bool ok; n = numeric_status(sh, argv[1], &ok); if (!ok || n < 1) return 2; }
  if ((flow == FLOW_BREAK || flow == FLOW_CONTINUE) && sh->loop_depth == 0) return 0;
  sh->flow = flow; sh->flow_count = n; return 0;
}
static int bi_break(Shell *s,int a,char**v){return flow_builtin(s,a,v,FLOW_BREAK);}
static int bi_continue(Shell*s,int a,char**v){return flow_builtin(s,a,v,FLOW_CONTINUE);}
static int bi_return(Shell *sh, int argc, char **argv) {
  if (!sh->function_depth) { fprintf(stderr, "%s: return: Illegal number or not in function\n", sh->name); return 2; }
  int rc = sh->last_status; if (argc > 1) { bool ok; rc = numeric_status(sh, argv[1], &ok); if (!ok) return 2; }
  sh->flow = FLOW_RETURN; sh->exit_status = rc; return rc;
}

static char decode_escape(const char **s, bool *stop) {
  char c = *(*s)++; *stop = false;
  switch (c) { case 'a': return '\a'; case 'b': return '\b'; case 'f': return '\f'; case 'n': return '\n'; case 'r': return '\r'; case 't': return '\t'; case 'v': return '\v'; case '\\': return '\\'; case 'c': *stop = true; return 0; default: (*s)--; return '\\'; }
}
static void print_b(const char *s, bool *stop) {
  while (*s && !*stop) { if (*s == '\\' && s[1]) { s++; if (*s >= '0' && *s <= '7') { int v = 0, k = 0; if (*s == '0') s++; while (k++ < 3 && *s >= '0' && *s <= '7') v = v * 8 + (*s++ - '0'); putchar(v); } else putchar(decode_escape(&s, stop)); } else putchar(*s++); }
}
static int bi_printf(Shell *sh, int argc, char **argv) {
  (void)sh; if (argc < 2) return 0; const char *format = argv[1]; int ai = 2; bool stop = false;
  do {
    for (const char *p = format; *p && !stop;) {
      if (*p == '\\') { p++; if (*p) putchar(decode_escape(&p, &stop)); else putchar('\\'); continue; }
      if (*p != '%') { putchar(*p++); continue; }
      p++; if (*p == '%') { putchar('%'); p++; continue; }
      char specbuf[64] = "%"; size_t z = 1;
      while (*p && strchr("-+ #0.123456789", *p) && z + 2 < sizeof(specbuf)) specbuf[z++] = *p++;
      char conv = *p ? *p++ : '\0'; specbuf[z++] = conv; specbuf[z] = '\0'; const char *arg = ai < argc ? argv[ai++] : "";
      if (conv == 's') printf(specbuf, arg); else if (conv == 'c') printf(specbuf, *arg);
      else if (conv == 'b') print_b(arg, &stop);
      else if (strchr("di", conv)) printf(specbuf, strtol(arg, NULL, 0));
      else if (strchr("ouxX", conv)) printf(specbuf, strtoul(arg, NULL, 0));
      else { fprintf(stderr, "printf: %%%c: invalid directive\n", conv); return 1; }
    }
  } while (!stop && ai < argc);
  return 0;
}

static int bi_echo(Shell *sh, int argc, char **argv) {
  (void)sh; int i = 1; bool newline = true; if (i < argc && !strcmp(argv[i], "-n")) { newline = false; i++; }
  for (; i < argc; i++) { if (i > 1 && !(i == 2 && !newline)) putchar(' '); fputs(argv[i], stdout); } if (newline) putchar('\n'); return 0;
}

static int bi_eval(Shell *sh, int argc, char **argv) {
  size_t n = 1; for (int i = 1; i < argc; i++) n += strlen(argv[i]) + 1; char *s = xmalloc(n); s[0] = '\0';
  for (int i = 1; i < argc; i++) { if (i > 1) strcat(s, " "); strcat(s, argv[i]); }
  int rc = execute_source(sh, s, "eval"); free(s); return rc;
}

static int bi_dot(Shell *sh, int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "%s: .: filename argument required\n", sh->name); return 2; }
  return execute_file(sh, argv[1], argv + 2, argc > 2 ? (size_t)(argc - 2) : 0, true);
}

static int bi_read(Shell *sh, int argc, char **argv) {
  bool raw = false; int i = 1; if (i < argc && !strcmp(argv[i], "-r")) { raw = true; i++; }
  if (i == argc) { fprintf(stderr, "%s: read: arg count\n", sh->name); return 2; }
  char *line = NULL; size_t cap = 0; ssize_t n;
  do n = getline(&line, &cap, stdin); while (n < 0 && errno == EINTR);
  if (n < 0) { free(line); return 1; } if (n && line[n - 1] == '\n') line[--n] = '\0';
  if (!raw) { size_t j = 0; for (size_t k = 0; line[k]; k++) if (line[k] == '\\' && line[k + 1]) line[j++] = line[++k]; else line[j++] = line[k]; line[j] = '\0'; }
  bool set; const char *ifs = shell_getvar(sh, "IFS", &set); if (!set) ifs = " \t\n"; char *at = line;
  for (; i < argc; i++) {
    while (*at && strchr(ifs, *at)) at++;
    char *value = at;
    if (i == argc - 1) at += strlen(at); else { while (*at && !strchr(ifs, *at)) at++; if (*at) *at++ = '\0'; }
    shell_setvar(sh, argv[i], value, false);
  }
  free(line); return 0;
}

static int bi_getopts(Shell *sh, int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "%s: getopts: usage: getopts optstring name [arg ...]\n", sh->name); return 2; }
  const char *optstring = argv[1];
  if (!valid_name(argv[2], strlen(argv[2]))) { fprintf(stderr, "%s: getopts: %s: bad variable name\n", sh->name, argv[2]); return 2; }
  bool set; const char *ind_s = shell_getvar(sh, "OPTIND", &set);
  char *end = NULL; long ind = set ? strtol(ind_s, &end, 10) : 1;
  if (!set || !*ind_s || (end && *end) || ind < 1) ind = 1;
  if (sh->getopts_index != ind) { sh->getopts_index = (int)ind; sh->getopts_offset = 1; }
  char **args = argc > 3 ? argv + 3 : sh->positional;
  size_t nargs = argc > 3 ? (size_t)(argc - 3) : sh->npositional;
  if ((size_t)sh->getopts_index > nargs) return 1;
  const char *arg = args[sh->getopts_index - 1];
  if (sh->getopts_offset == 1 && (!strcmp(arg, "--") || arg[0] != '-' || !arg[1])) {
    if (!strcmp(arg, "--")) sh->getopts_index++;
    char buf[32]; snprintf(buf, sizeof(buf), "%d", sh->getopts_index); shell_setvar(sh, "OPTIND", buf, false);
    return 1;
  }
  char option = arg[sh->getopts_offset++];
  const char *spec = strchr(optstring + (*optstring == ':'), option);
  char value[2] = {option, '\0'};
  if (!spec || option == ':') {
    shell_setvar(sh, argv[2], "?", false); shell_setvar(sh, "OPTARG", value, false);
    if (*optstring != ':') fprintf(stderr, "%s: Illegal option -%c\n", sh->name, option);
  } else if (spec[1] == ':') {
    const char *optarg = NULL;
    if (arg[sh->getopts_offset]) { optarg = arg + sh->getopts_offset; sh->getopts_index++; sh->getopts_offset = 1; }
    else if ((size_t)sh->getopts_index < nargs) { optarg = args[sh->getopts_index]; sh->getopts_index += 2; sh->getopts_offset = 1; }
    else {
      shell_setvar(sh, argv[2], *optstring == ':' ? ":" : "?", false);
      shell_setvar(sh, "OPTARG", value, false);
      if (*optstring != ':') fprintf(stderr, "%s: No arg for -%c option\n", sh->name, option);
      goto update;
    }
    shell_setvar(sh, argv[2], value, false); shell_setvar(sh, "OPTARG", optarg, false);
  } else {
    shell_setvar(sh, argv[2], value, false); shell_unsetvar(sh, "OPTARG");
  }
  if (!arg[sh->getopts_offset]) { sh->getopts_index++; sh->getopts_offset = 1; }
update: {
    char buf[32]; snprintf(buf, sizeof(buf), "%d", sh->getopts_index); shell_setvar(sh, "OPTIND", buf, false);
  }
  return 0;
}

static int bi_umask(Shell *sh, int argc, char **argv) {
  (void)sh; if (argc == 1) { mode_t m = umask(0); umask(m); printf("%04o\n", (unsigned)m); return 0; }
  char *end; long m = strtol(argv[1], &end, 8); if (*end || m < 0 || m > 0777) { fprintf(stderr, "umask: Illegal mode: %s\n", argv[1]); return 1; } umask((mode_t)m); return 0;
}

static void print_alias(const char *name, const char *value) {
  printf("%s='", name); for (const char *p = value; *p; p++) { if (*p == '\'') fputs("'\\''", stdout); else putchar(*p); } puts("'");
}
static int bi_alias(Shell *sh, int argc, char **argv) {
  if (argc == 1) { for (Alias *a = sh->aliases; a; a = a->next) print_alias(a->name, a->value); return 0; }
  int rc = 0; for (int i = 1; i < argc; i++) { char *eq = strchr(argv[i], '='); if (!eq) { const char *v = shell_getalias(sh, argv[i]); if (v) print_alias(argv[i], v); else { fprintf(stderr, "alias: %s not found\n", argv[i]); rc = 1; } } else { char *name = xstrndup(argv[i], (size_t)(eq - argv[i])); if (!valid_name(name, strlen(name))) { fprintf(stderr, "alias: %s: invalid alias name\n", name); rc = 1; } else shell_setalias(sh, name, eq + 1); free(name); } } return rc;
}
static int bi_unalias(Shell *sh, int argc, char **argv) {
  if (argc < 2) return 2;
  if (!strcmp(argv[1], "-a")) { while (sh->aliases) shell_unsetalias(sh, sh->aliases->name); return 0; }
  int rc = 0; for (int i = 1; i < argc; i++) rc |= shell_unsetalias(sh, argv[i]); return rc;
}

static int bi_times(Shell *sh, int argc, char **argv) {
  (void)sh;(void)argc;(void)argv; struct tms t; long hz = sysconf(_SC_CLK_TCK); times(&t);
  printf("%ldm%.3fs %ldm%.3fs\n", t.tms_utime/hz, (double)(t.tms_utime%hz)/hz, t.tms_stime/hz, (double)(t.tms_stime%hz)/hz);
  printf("%ldm%.3fs %ldm%.3fs\n", t.tms_cutime/hz, (double)(t.tms_cutime%hz)/hz, t.tms_cstime/hz, (double)(t.tms_cstime%hz)/hz); return 0;
}

static int wait_one(Shell *sh, pid_t pid) {
  (void)sh;
  int st; while (waitpid(pid, &st, 0) < 0) { if (errno == EINTR) continue; if (errno == ECHILD) return 127; perror("wait"); return 1; }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return 1;
}
static int bi_wait(Shell *sh, int argc, char **argv) {
  int rc = 0;
  if (argc > 1) { for (int i=1;i<argc;i++){char*e;long p=strtol(argv[i],&e,10);if(*e){fprintf(stderr,"%s: wait: %s: Illegal number\n",sh->name,argv[i]);rc=2;}else rc=wait_one(sh,(pid_t)p);} return rc; }
  for (Job *j = sh->jobs; j; j = j->next) if (!j->done) { rc = wait_one(sh, j->pid); j->done = true; j->status = rc; }
  return rc;
}

static int bi_jobs(Shell *sh, int argc, char **argv) {
  (void)argc;(void)argv; reap_jobs(sh, false); for (Job*j=sh->jobs;j;j=j->next) printf("[%d] %s %s\n",j->id,j->done?"Done":"Running",j->command); return 0;
}

static int bi_command(Shell *sh, int argc, char **argv) {
  int i=1; bool query=false,default_path=false; while(i<argc&&argv[i][0]=='-'){if(!strcmp(argv[i],"-v")||!strcmp(argv[i],"-V"))query=true;else if(!strcmp(argv[i],"-p"))default_path=true;else break;i++;}
  if(i==argc)return 0;
  bool special; BuiltinFn fn=builtin_lookup(argv[i],&special); (void)special;
  const char *search_path=default_path?"/bin:/usr/bin":getenv("PATH");
  if(query){if(fn){puts(argv[i]);return 0;}char*copy=xstrdup(search_path?search_path:"/bin:/usr/bin"),*save=NULL;for(char*d=strtok_r(copy,":",&save);d;d=strtok_r(NULL,":",&save)){char b[4096];snprintf(b,sizeof(b),"%s/%s",*d?d:".",argv[i]);if(access(b,X_OK)==0){puts(b);free(copy);return 0;}}free(copy);return 1;}
  if(fn)return fn(sh,argc-i,argv+i);
  pid_t p=fork();if(p==0){if(default_path){char*copy=xstrdup(search_path),*save=NULL;for(char*d=strtok_r(copy,":",&save);d;d=strtok_r(NULL,":",&save)){char b[4096];snprintf(b,sizeof(b),"%s/%s",*d?d:".",argv[i]);execv(b,argv+i);if(errno!=ENOENT&&errno!=ENOTDIR)_exit(126);}free(copy);_exit(127);}execvp(argv[i],argv+i);_exit(errno==ENOENT?127:126);}if(p<0)return 1;return wait_one(sh,p);
}
static int bi_type(Shell *sh,int argc,char**argv){if(argc<2)return 0;char*av[4]={"command","-V",argv[1],NULL};return bi_command(sh,3,av);}

static int bi_exec(Shell *sh, int argc, char **argv) {
  if (argc == 1) return 0;
  execvp(argv[1], argv + 1); int e=errno; fprintf(stderr,"%s: exec: %s: %s\n",sh->name,argv[1],strerror(e)); return e==ENOENT?127:126;
}

static int bi_trap(Shell *sh, int argc, char **argv) {
  if(argc==1){if(sh->trap_exit)printf("trap -- '%s' EXIT\n",sh->trap_exit);if(sh->trap_int)printf("trap -- '%s' INT\n",sh->trap_int);return 0;}
  if(argc<3)return 2;
  for(int i=2;i<argc;i++){char **slot=NULL;int sig=0;if(!strcmp(argv[i],"0")||!strcmp(argv[i],"EXIT"))slot=&sh->trap_exit;else if(!strcmp(argv[i],"2")||!strcmp(argv[i],"INT")){slot=&sh->trap_int;sig=SIGINT;}else if(!strcmp(argv[i],"15")||!strcmp(argv[i],"TERM")){slot=&sh->trap_term;sig=SIGTERM;}else if(!strcmp(argv[i],"1")||!strcmp(argv[i],"HUP")){slot=&sh->trap_hup;sig=SIGHUP;}else{fprintf(stderr,"trap: %s: bad trap\n",argv[i]);continue;}free(*slot);*slot=*argv[1]?xstrdup(argv[1]):NULL;if(sig)shell_configure_signal(sig,*argv[1]!=0);}
  return 0;
}

static int bi_help(Shell *sh,int argc,char**argv){(void)sh;(void)argc;(void)argv;puts("lsh: a small POSIX-oriented shell; use 'type name' to inspect commands");return 0;}

typedef struct { const char *name; BuiltinFn fn; bool special; } Builtin;
static const Builtin builtins[] = {
  {".",bi_dot,true},{":",bi_colon,true},{"break",bi_break,true},{"continue",bi_continue,true},
  {"eval",bi_eval,true},{"exec",bi_exec,true},{"exit",bi_exit,true},{"export",bi_export,true},
  {"readonly",bi_readonly,true},{"return",bi_return,true},{"set",bi_set,true},{"shift",bi_shift,true},
  {"times",bi_times,true},{"trap",bi_trap,true},{"unset",bi_unset,true},
  {"alias",bi_alias,false},{"cd",bi_cd,false},{"command",bi_command,false},{"echo",bi_echo,false},{"false",bi_false,false},{"getopts",bi_getopts,false},
  {"help",bi_help,false},{"jobs",bi_jobs,false},{"printf",bi_printf,false},{"pwd",bi_pwd,false},
  {"read",bi_read,false},{"true",bi_colon,false},{"type",bi_type,false},{"umask",bi_umask,false},{"unalias",bi_unalias,false},{"wait",bi_wait,false}
};

BuiltinFn builtin_lookup(const char *name, bool *special) {
  for(size_t i=0;i<sizeof(builtins)/sizeof(builtins[0]);i++)if(!strcmp(name,builtins[i].name)){if(special)*special=builtins[i].special;return builtins[i].fn;}
  if(special)*special=false;
  return NULL;
}
int builtin_run(Shell *sh, BuiltinFn fn, int argc, char **argv) { return fn(sh,argc,argv); }
