#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { char *name, *value; bool was_set, exported; } SavedVar;

static volatile sig_atomic_t pending_signal;
static void trap_handler(int sig) { pending_signal = sig; }
void shell_configure_signal(int sig, bool trapped) {
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_handler = trapped ? trap_handler : SIG_DFL;
  sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
  sigaction(sig, &sa, NULL);
}

static void run_pending_trap(Shell *sh) {
  int sig = pending_signal; if (!sig) return; pending_signal = 0;
  const char *action = sig == SIGINT ? sh->trap_int : sig == SIGTERM ? sh->trap_term : sig == SIGHUP ? sh->trap_hup : NULL;
  if (action) { int saved = sh->last_status; execute_source(sh, action, "trap"); sh->last_status = saved; }
}

static int status_value(int st) {
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return 1;
}

static int wait_for(pid_t pid) {
  int st;
  while (waitpid(pid, &st, 0) < 0) { if (errno == EINTR) continue; return 1; }
  return status_value(st);
}

static bool assignment_spelling(const char *word, size_t *eq_at) {
  bool sq = false, dq = false;
  for (size_t i = 0; word[i]; i++) {
    if (sq) { if (word[i] == '\'') sq = false; continue; }
    if (dq) { if (word[i] == '"') dq = false; else if (word[i] == '\\' && word[i+1]) i++; continue; }
    if (word[i] == '\'') sq = true; else if (word[i] == '"') dq = true; else if (word[i] == '\\' && word[i+1]) i++;
    else if (word[i] == '=') { if (valid_name(word, i)) { *eq_at = i; return true; } return false; }
  }
  return false;
}

static SavedVar save_var(Shell *sh, const char *name) {
  SavedVar s = {.name=xstrdup(name)}; bool set; const char *v=shell_getvar(sh,name,&set); s.was_set=set; s.value=xstrdup(v);
  for(Variable*x=sh->vars;x;x=x->next)if(!strcmp(x->name,name)){s.exported=x->exported;return s;}
  s.exported=getenv(name)!=NULL; return s;
}
static void restore_var(Shell *sh, SavedVar *s) {
  if (!s->name) return;
  if(s->was_set)shell_setvar(sh,s->name,s->value,s->exported);else shell_unsetvar(sh,s->name);
  free(s->name);free(s->value);
}

static int make_herefile(const char *body) {
  char path[] = "/tmp/lsh-heredoc-XXXXXX"; int fd = mkstemp(path); if(fd<0)return -1; unlink(path);
  size_t n=strlen(body),off=0;while(off<n){ssize_t w=write(fd,body+off,n-off);if(w<0){if(errno==EINTR)continue;close(fd);return -1;}off+=(size_t)w;}
  if(lseek(fd,0,SEEK_SET)<0){close(fd);return -1;}return fd;
}

static int apply_redirs(Shell *sh, Redir *r, int **saved_out, size_t *nsaved) {
  int *saved=NULL; size_t nsave=0;
  for(;r;r=r->next){
    int old=-2;
    if(saved_out){old=dup(r->fd);if(old<0&&errno!=EBADF){perror("lsh: dup");goto fail;}saved=xrealloc(saved,(nsave+2)*sizeof(int));saved[nsave++]=r->fd;saved[nsave++]=old;}
    int source=-1; char *target=NULL;
    if(r->type==REDIR_HEREDOC){char *body=r->heredoc_expand?expand_heredoc(sh,r->heredoc):xstrdup(r->heredoc);source=make_herefile(body);free(body);}
    else {Fields f=expand_word(sh,r->word,false,false);if(f.n!=1){fprintf(stderr,"%s: %s: ambiguous redirect\n",sh->name,r->word);fields_free(&f);goto fail;}target=xstrdup(f.v[0]);fields_free(&f);
      switch(r->type){case REDIR_IN:source=open(target,O_RDONLY);break;case REDIR_OUT:source=open(target,O_WRONLY|O_CREAT|O_TRUNC,0666);break;case REDIR_APPEND:source=open(target,O_WRONLY|O_CREAT|O_APPEND,0666);break;case REDIR_INOUT:source=open(target,O_RDWR|O_CREAT,0666);break;
        case REDIR_DUP_IN:case REDIR_DUP_OUT:if(!strcmp(target,"-")){if(close(r->fd)<0&&errno!=EBADF){perror("lsh: close");free(target);goto fail;}free(target);continue;}else{char*e;long x=strtol(target,&e,10);if(!*target||*e||x<0){fprintf(stderr,"%s: %s: bad file descriptor\n",sh->name,target);free(target);goto fail;}source=dup((int)x);}break;default:break;}}
    if(source<0){fprintf(stderr,"%s: %s: %s\n",sh->name,target?target:"here-document",strerror(errno));free(target);goto fail;}
    if(source!=r->fd){if(dup2(source,r->fd)<0){perror("lsh: dup2");close(source);free(target);goto fail;}close(source);} free(target);
  }
  if(saved_out){*saved_out=saved;*nsaved=nsave;}return 0;
fail:
  if(saved){for(size_t i=nsave;i>=2;i-=2){int fd=saved[i-2],old=saved[i-1];if(old>=0){dup2(old,fd);close(old);}else close(fd);}free(saved);}return 1;
}

static void restore_redirs(int *saved,size_t n){for(size_t i=n;i>=2;i-=2){int fd=saved[i-2],old=saved[i-1];if(old>=0){dup2(old,fd);close(old);}else close(fd);}free(saved);}

static void trace_argv(Shell *sh,int argc,char**argv){if(!sh->opt_xtrace)return;fputs("+",stderr);for(int i=0;i<argc;i++)fprintf(stderr," %s",argv[i]);fputc('\n',stderr);}

static int exec_external(Shell *sh,int argc,char**argv,bool child,Redir*redirs) {
  if(child){signal(SIGINT,SIG_DFL);signal(SIGQUIT,SIG_DFL);if(apply_redirs(sh,redirs,NULL,NULL))return 1;execvp(argv[0],argv);
    int e=errno;if(e==ENOEXEC){int rc=execute_file(sh,argv[0],argv+1,(size_t)(argc-1),false);return rc;}
    fprintf(stderr,"%s: %s: %s\n",sh->name,argv[0],strerror(e));return e==ENOENT?127:126;}
  fflush(NULL);pid_t pid=fork();if(pid<0){perror("lsh: fork");return 1;}if(pid==0)_exit(exec_external(sh,argc,argv,true,redirs));return wait_for(pid);
}

static Function *find_function(Shell *sh,const char*name){for(Function*f=sh->functions;f;f=f->next)if(!strcmp(f->name,name))return f;return NULL;}

static void set_positionals_owned(Shell*sh,char**v,size_t n){sh->positional=n?xmalloc(n*sizeof(char*)):NULL;sh->npositional=n;for(size_t i=0;i<n;i++)sh->positional[i]=xstrdup(v[i]);}

static int run_function(Shell *sh,Function*f,int argc,char**argv,Redir*call_redirs) {
  char **old=sh->positional;size_t oldn=sh->npositional;set_positionals_owned(sh,argv+1,(size_t)(argc-1));
  int *saved_def=NULL,*saved_call=NULL;size_t ns_def=0,ns_call=0;
  if(apply_redirs(sh,f->redirs,&saved_def,&ns_def)||apply_redirs(sh,call_redirs,&saved_call,&ns_call)){
    if(saved_def)restore_redirs(saved_def,ns_def);
    for(size_t i=0;i<sh->npositional;i++)free(sh->positional[i]);
    free(sh->positional);
    sh->positional=old;sh->npositional=oldn;return 1;
  }
  sh->function_depth++;int rc=execute_ast(sh,f->body,false);sh->function_depth--;
  if(sh->flow==FLOW_RETURN){rc=sh->exit_status;sh->flow=FLOW_NONE;}
  restore_redirs(saved_call,ns_call);restore_redirs(saved_def,ns_def);
  for(size_t i=0;i<sh->npositional;i++)free(sh->positional[i]);
  free(sh->positional);sh->positional=old;sh->npositional=oldn;return rc;
}

static int execute_simple(Shell *sh,Ast *n,bool child) {
  size_t nassign=0;
  while(nassign<n->u.simple.nwords){size_t eq;if(!assignment_spelling(n->u.simple.words[nassign],&eq))break;nassign++;}
  char **anames=nassign?xmalloc(nassign*sizeof(char*)):NULL,**avals=nassign?xmalloc(nassign*sizeof(char*)):NULL;
  for(size_t i=0;i<nassign;i++){size_t eq;assignment_spelling(n->u.simple.words[i],&eq);anames[i]=xstrndup(n->u.simple.words[i],eq);avals[i]=expand_string(sh,n->u.simple.words[i]+eq+1);}
  char **argv=NULL;int argc=0;
  for(size_t i=nassign;i<n->u.simple.nwords;i++){Fields f=expand_word(sh,n->u.simple.words[i],true,false);for(size_t k=0;k<f.n;k++){argv=xrealloc(argv,(size_t)(argc+2)*sizeof(char*));argv[argc++]=xstrdup(f.v[k]);argv[argc]=NULL;}fields_free(&f);}
  if(sh->expansion_failed){sh->expansion_failed=false;if(!sh->interactive){sh->should_exit=true;sh->exit_status=2;sh->flow=FLOW_EXIT;}for(size_t i=0;i<nassign;i++){free(anames[i]);free(avals[i]);}free(anames);free(avals);for(int i=0;i<argc;i++)free(argv[i]);free(argv);return 2;}
  bool special=false;BuiltinFn bi=argc?builtin_lookup(argv[0],&special):NULL;Function *fn=argc?find_function(sh,argv[0]):NULL;
  bool persist=!argc||special;SavedVar *saved=(!persist&&nassign)?xmalloc(nassign*sizeof(*saved)):NULL;
  bool assignment_error=false;
  for(size_t i=0;i<nassign;i++){if(saved)saved[i]=save_var(sh,anames[i]);bool pass_environment=child||(!bi&&!fn)||(argc&&(!strcmp(argv[0],"command")||!strcmp(argv[0],"exec")));if(shell_setvar(sh,anames[i],avals[i],pass_environment)){assignment_error=true;if(saved){free(saved[i].name);free(saved[i].value);memset(&saved[i],0,sizeof(saved[i]));}}}
  int rc=0;trace_argv(sh,argc,argv);
  if(assignment_error){rc=2;if(!sh->interactive){sh->should_exit=true;sh->exit_status=2;sh->flow=FLOW_EXIT;}}
  else if(!argc){int *fds=NULL;size_t nf=0;rc=apply_redirs(sh,n->redirs,child?NULL:&fds,child?NULL:&nf);if(fds)restore_redirs(fds,nf);if(rc&&!sh->interactive){sh->should_exit=true;sh->exit_status=rc;sh->flow=FLOW_EXIT;}}
  else if(fn)rc=run_function(sh,fn,argc,argv,n->redirs);
  else if(bi){int *fds=NULL;size_t nf=0;bool persistent_exec=!strcmp(argv[0],"exec")&&argc==1;
    if(apply_redirs(sh,n->redirs,(child||persistent_exec)?NULL:&fds,(child||persistent_exec)?NULL:&nf)){rc=1;if(special&&!sh->interactive){sh->should_exit=true;sh->exit_status=rc;sh->flow=FLOW_EXIT;}}else rc=builtin_run(sh,bi,argc,argv);
    fflush(NULL);if(fds)restore_redirs(fds,nf);}
  else rc=exec_external(sh,argc,argv,child,n->redirs);
  if(saved){for(size_t i=nassign;i>0;i--)restore_var(sh,&saved[i-1]);free(saved);}
  for(size_t i=0;i<nassign;i++){free(anames[i]);free(avals[i]);}free(anames);free(avals);for(int i=0;i<argc;i++)free(argv[i]);free(argv);return rc;
}

static int execute_pipeline(Shell *sh,Ast*n) {
  size_t count=n->u.pipeline.ncommands;pid_t*pids=xmalloc(count*sizeof(pid_t));int prev=-1;pid_t pgid=0;
  fflush(NULL);
  for(size_t i=0;i<count;i++){int fds[2]={-1,-1};if(i+1<count&&pipe(fds)<0){perror("lsh: pipe");free(pids);return 1;}pid_t p=fork();if(p<0){perror("lsh: fork");free(pids);return 1;}if(p==0){if(!pgid)pgid=getpid();setpgid(0,pgid);signal(SIGINT,SIG_DFL);if(prev>=0){dup2(prev,STDIN_FILENO);}if(i+1<count)dup2(fds[1],STDOUT_FILENO);if(prev>=0)close(prev);if(fds[0]>=0)close(fds[0]);if(fds[1]>=0)close(fds[1]);int rc=execute_ast(sh,n->u.pipeline.commands[i],true);fflush(NULL);_exit(rc);}if(!pgid)pgid=p;setpgid(p,pgid);pids[i]=p;if(prev>=0)close(prev);if(fds[1]>=0)close(fds[1]);prev=fds[0];}
  if(prev>=0)close(prev);
  int rc=0;for(size_t i=0;i<count;i++){int x=wait_for(pids[i]);if(i+1==count)rc=x;}free(pids);return rc;
}

static int execute_subshell(Shell*sh,Ast*body,Redir*r){fflush(NULL);pid_t p=fork();if(p<0)return 1;if(p==0){if(apply_redirs(sh,r,NULL,NULL))_exit(1);int rc=execute_ast(sh,body,true);fflush(NULL);_exit(rc);}return wait_for(p);}

static int execute_group(Shell*sh,Ast*body,Redir*r,bool child){int*saved=NULL;size_t n=0;if(apply_redirs(sh,r,child?NULL:&saved,child?NULL:&n))return 1;int rc=execute_ast(sh,body,child);if(saved)restore_redirs(saved,n);return rc;}

static int execute_case(Shell*sh,Ast*n,bool child){char*word=expand_string(sh,n->u.case_clause.word);int rc=0;for(CaseArm*a=n->u.case_clause.arms;a;a=a->next){bool match=false;for(size_t i=0;i<a->npatterns&&!match;i++){char*p=expand_case_pattern(sh,a->patterns[i]);match=fnmatch(p,word,0)==0;free(p);}if(match){rc=execute_ast(sh,a->body,child);break;}}free(word);return rc;}

int execute_ast(Shell *sh,Ast *n,bool child) {
  run_pending_trap(sh);
  if(!n||sh->flow!=FLOW_NONE)return sh->last_status;
  int rc=0;
  switch(n->type){
    case AST_SIMPLE:rc=execute_simple(sh,n,child);break;
    case AST_SEQUENCE:rc=execute_ast(sh,n->u.binary.left,child);if(sh->flow==FLOW_NONE)rc=execute_ast(sh,n->u.binary.right,child);break;
    case AST_AND:{bool e=sh->opt_errexit;sh->opt_errexit=false;rc=execute_ast(sh,n->u.binary.left,child);sh->opt_errexit=e;if(rc==0&&sh->flow==FLOW_NONE)rc=execute_ast(sh,n->u.binary.right,child);break;}
    case AST_OR:{bool e=sh->opt_errexit;sh->opt_errexit=false;rc=execute_ast(sh,n->u.binary.left,child);sh->opt_errexit=e;if(rc!=0&&sh->flow==FLOW_NONE)rc=execute_ast(sh,n->u.binary.right,child);break;}
    case AST_NOT:{bool e=sh->opt_errexit;sh->opt_errexit=false;rc=!execute_ast(sh,n->u.unary.body,child);sh->opt_errexit=e;break;}
    case AST_PIPELINE:rc=execute_pipeline(sh,n);break;
    case AST_SUBSHELL:rc=execute_subshell(sh,n->u.unary.body,n->redirs);break;
    case AST_GROUP:rc=execute_group(sh,n->u.unary.body,n->redirs,child);break;
    case AST_BACKGROUND:{fflush(NULL);pid_t p=fork();if(p<0){rc=1;break;}if(p==0){setpgid(0,0);signal(SIGINT,SIG_IGN);if(!sh->interactive){int fd=open("/dev/null",O_RDONLY);if(fd>=0){dup2(fd,0);close(fd);}}int x=execute_ast(sh,n->u.unary.body,true);fflush(NULL);_exit(x);}setpgid(p,p);sh->last_bg=p;Job*j=xmalloc(sizeof(*j));*j=(Job){sh->next_job++,p,p,ast_command_name(n->u.unary.body),0,false,sh->jobs};sh->jobs=j;if(sh->interactive)fprintf(stderr,"[%d] %ld\n",j->id,(long)p);rc=0;break;}
    case AST_IF:{bool e=sh->opt_errexit;sh->opt_errexit=false;rc=execute_ast(sh,n->u.if_clause.condition,child);sh->opt_errexit=e;if(rc==0)rc=execute_ast(sh,n->u.if_clause.then_part,child);else if(n->u.if_clause.else_part)rc=execute_ast(sh,n->u.if_clause.else_part,child);else rc=0;break;}
    case AST_WHILE:case AST_UNTIL:{rc=0;sh->loop_depth++;for(;;){bool e=sh->opt_errexit;sh->opt_errexit=false;int c=execute_ast(sh,n->u.loop.condition,child);sh->opt_errexit=e;if((n->type==AST_WHILE&&c!=0)||(n->type==AST_UNTIL&&c==0))break;rc=execute_ast(sh,n->u.loop.body,child);if(sh->flow==FLOW_BREAK){if(--sh->flow_count<=0)sh->flow=FLOW_NONE;break;}if(sh->flow==FLOW_CONTINUE){if(--sh->flow_count<=0)sh->flow=FLOW_NONE;else break;}if(sh->flow!=FLOW_NONE)break;}sh->loop_depth--;break;}
    case AST_FOR:{Fields all={0};if(!n->u.for_clause.nwords){all.n=sh->npositional;all.v=all.n?xmalloc(all.n*sizeof(char*)):NULL;for(size_t i=0;i<all.n;i++)all.v[i]=xstrdup(sh->positional[i]);}else for(size_t i=0;i<n->u.for_clause.nwords;i++){Fields f=expand_word(sh,n->u.for_clause.words[i],true,false);for(size_t k=0;k<f.n;k++){all.v=xrealloc(all.v,(all.n+1)*sizeof(char*));all.v[all.n++]=xstrdup(f.v[k]);}fields_free(&f);}sh->loop_depth++;rc=0;for(size_t i=0;i<all.n;i++){shell_setvar(sh,n->u.for_clause.name,all.v[i],false);rc=execute_ast(sh,n->u.for_clause.body,child);if(sh->flow==FLOW_BREAK){if(--sh->flow_count<=0)sh->flow=FLOW_NONE;break;}if(sh->flow==FLOW_CONTINUE){if(--sh->flow_count<=0)sh->flow=FLOW_NONE;else break;}if(sh->flow!=FLOW_NONE)break;}sh->loop_depth--;fields_free(&all);break;}
    case AST_CASE:rc=execute_case(sh,n,child);break;
    case AST_FUNCTION:{Function**at=&sh->functions;while(*at&&strcmp((*at)->name,n->u.function.name))at=&(*at)->next;if(*at){(*at)->body=n->u.function.body;(*at)->redirs=n->redirs;}else{Function*f=xmalloc(sizeof(*f));*f=(Function){xstrdup(n->u.function.name),n->u.function.body,n->redirs,sh->functions};sh->functions=f;}rc=0;break;}
  }
  sh->last_status=rc;
  run_pending_trap(sh);
  bool errexit_here=n->type!=AST_AND&&n->type!=AST_OR&&n->type!=AST_NOT&&n->type!=AST_IF&&n->type!=AST_WHILE&&n->type!=AST_UNTIL;
  if(errexit_here&&sh->opt_errexit&&!sh->interactive&&rc!=0&&sh->flow==FLOW_NONE){sh->should_exit=true;sh->exit_status=rc;sh->flow=FLOW_EXIT;}
  return rc;
}

static int execute_complete(Shell *sh,const char*source,const char*label){TokenList t=lex_source(source);alias_expand_tokens(sh,&t);char*error=NULL;bool incomplete=false;Ast*root=parse_tokens(&t,&error,&incomplete);if(error){fprintf(stderr,"%s: %s: %s%s\n",sh->name,label?label:"input",error,incomplete?" (unexpected EOF)":"");free(error);token_list_free(&t);sh->last_status=2;return 2;}token_list_free(&t);if(!root)return sh->last_status;AstOwner*o=xmalloc(sizeof(*o));*o=(AstOwner){root,sh->asts};sh->asts=o;return execute_ast(sh,root,false);}

int execute_source(Shell *sh,const char*source,const char*label){size_t start=0,n=strlen(source);int rc=sh->last_status;for(size_t i=0;i<=n;i++){if(i<n&&source[i]!='\n')continue;size_t z=i-start+(i<n);char*chunk=xstrndup(source+start,z);TokenList t=lex_source(chunk);char*error=NULL;bool incomplete=false;Ast*a=parse_tokens(&t,&error,&incomplete);ast_free(a);token_list_free(&t);if(incomplete&&i<n){free(error);free(chunk);continue;}bool syntax_error=error!=NULL;free(error);rc=execute_complete(sh,chunk,label);free(chunk);start=i+1;if(syntax_error||sh->should_exit||sh->flow!=FLOW_NONE)break;}return rc;}

static char *find_source(Shell*sh,const char*path){if(strchr(path,'/'))return xstrdup(path);bool set;const char*p=shell_getvar(sh,"PATH",&set);char*copy=xstrdup(set?p:"/bin:/usr/bin"),*save=NULL;for(char*d=strtok_r(copy,":",&save);d;d=strtok_r(NULL,":",&save)){size_t n=strlen(d)+strlen(path)+2;char*f=xmalloc(n);snprintf(f,n,"%s%s%s",d,*d?"/":"",path);if(access(f,R_OK)==0){free(copy);return f;}free(f);}free(copy);return xstrdup(path);}

int execute_file(Shell*sh,const char*path,char**argv,size_t argc,bool dot){char*actual=dot?find_source(sh,path):xstrdup(path);FILE*f=fopen(actual,"r");if(!f){fprintf(stderr,"%s: %s: %s\n",sh->name,path,strerror(errno));free(actual);return errno==ENOENT?127:126;}char*source=read_all(f);fclose(f);char**old=sh->positional;size_t oldn=sh->npositional;if(!dot||argc){set_positionals_owned(sh,argv,argc);}int rc=execute_source(sh,source,actual);if(!dot||argc){for(size_t i=0;i<sh->npositional;i++)free(sh->positional[i]);free(sh->positional);sh->positional=old;sh->npositional=oldn;}free(source);free(actual);return rc;}

int command_substitute(Shell*sh,const char*source,char**output){int p[2];if(pipe(p)<0){*output=xstrdup("");return 1;}fflush(NULL);pid_t pid=fork();if(pid<0){close(p[0]);close(p[1]);*output=xstrdup("");return 1;}if(pid==0){close(p[0]);dup2(p[1],1);close(p[1]);int rc=execute_source(sh,source,"command substitution");fflush(NULL);_exit(rc);}close(p[1]);size_t n=0,cap=1024;char*b=xmalloc(cap);for(;;){if(n+512+1>cap){cap*=2;b=xrealloc(b,cap);}ssize_t z=read(p[0],b+n,cap-n-1);if(z<0&&errno==EINTR)continue;if(z<=0)break;n+=(size_t)z;}close(p[0]);int rc=wait_for(pid);while(n&&b[n-1]=='\n')n--;b[n]='\0';*output=b;sh->last_status=rc;return rc;}

void reap_jobs(Shell*sh,bool notify){for(Job*j=sh->jobs;j;j=j->next)if(!j->done){int st;pid_t p=waitpid(j->pid,&st,WNOHANG);if(p==j->pid){j->done=true;j->status=status_value(st);if(notify&&sh->interactive)fprintf(stderr,"[%d] Done %s\n",j->id,j->command);}}}
