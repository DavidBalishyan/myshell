#include "shell.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *append_line(char *source,size_t *len,const char *line){size_t n=strlen(line);source=xrealloc(source,*len+n+1);memcpy(source+*len,line,n+1);*len+=n;return source;}

static int interactive_loop(Shell *sh){char*source=NULL;size_t len=0;int rc=0;for(;;){reap_jobs(sh,true);fputs(len?"> ":"$ ",stderr);fflush(stderr);char*line=NULL;size_t cap=0;ssize_t n=getline(&line,&cap,stdin);if(n<0){free(line);if(len)rc=execute_source(sh,source,"input");break;}source=append_line(source,&len,line);free(line);TokenList t=lex_source(source);char*error=NULL;bool incomplete=false;Ast*a=parse_tokens(&t,&error,&incomplete);ast_free(a);token_list_free(&t);free(error);if(incomplete)continue;rc=execute_source(sh,source,"input");free(source);source=NULL;len=0;if(sh->should_exit)break;}free(source);return rc;}

static void set_args(Shell*sh,char**argv,int argc){sh->npositional=argc>0?(size_t)argc:0;sh->positional=sh->npositional?xmalloc(sh->npositional*sizeof(char*)):NULL;for(size_t i=0;i<sh->npositional;i++)sh->positional[i]=xstrdup(argv[i]);}

int main(int argc,char**argv){bool command=false;const char*input=NULL;int argi=1;
  while(argi<argc&&argv[argi][0]=='-'&&argv[argi][1]){if(!strcmp(argv[argi],"--")){argi++;break;}if(!strcmp(argv[argi],"-c")){command=true;if(++argi>=argc){fprintf(stderr,"lsh: -c requires an argument\n");return 2;}input=argv[argi++];break;}fprintf(stderr,"lsh: illegal option -- %s\n",argv[argi]);return 2;}
  const char *script = NULL;
  if (!command && argi < argc) script = argv[argi++];
  const char *name = command ? (argi < argc ? argv[argi++] : argv[0]) : (script ? script : argv[0]);
  bool interactive = !command && !script && isatty(STDIN_FILENO);
  Shell sh;shell_init(&sh,name,interactive);set_args(&sh,argv+argi,argc-argi);
  if(interactive){signal(SIGINT,SIG_IGN);signal(SIGQUIT,SIG_IGN);}int rc;
  if(interactive)rc=interactive_loop(&sh);
  else if(command)rc=execute_source(&sh,input,"-c");
  else if(script)rc=execute_file(&sh,script,sh.positional,sh.npositional,false);
  else{char*source=read_all(stdin);rc=execute_source(&sh,source,"stdin");free(source);}
  if (sh.should_exit) rc = sh.exit_status;
  if (sh.trap_exit) {
    char *action = xstrdup(sh.trap_exit);
    sh.flow = FLOW_NONE; sh.should_exit = false; sh.last_status = rc;
    execute_source(&sh, action, "EXIT trap"); free(action);
  }
  shell_destroy(&sh);
  return rc;
}
