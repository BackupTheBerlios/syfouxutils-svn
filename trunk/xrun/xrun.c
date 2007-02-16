/*--- xrun.c ----------------------------------------------------------------
Terminal utility that gives a *fast* way to build and launch arbitrary shell
commands, coming complete with readline support, command history management,
auto-completion, and xterm-compatible pseudo-terminal resize. It is a highly
configurable utility, see `-h` for help:

------------
sh # xrun -h
------------

Author's comments
~~~~~~~~~~~~~~~~~
In fact, this is not even a X program: that's a pure console mini-shell that was
meant to be used in conjunction with a pseudo-terminal emulator (xterm,
rxvt-unicode, etc.) as a fast, very configurable application launcher: plenty of
them exist, often bundled with window managers, but I got tired of half-baked,
semi-working solutions with no solid keybindings or incomplete history
support... This one is certainly not flashy, but it delivers in term of
efficiency to all of you who are familiar with Readline, either through your
shell, emacs, or your favorite interpreted language.

Requirements
~~~~~~~~~~~~
- A reasonnably POSIX compliant system
- A working C compiler with the basic libraries (this code is C90)
- The autotools (built on autoconf 2.61 and automake 1.10) 
- A terminal management library (ncurses, termcap or curses)
- GNU Readline with history support (tested on GNU Readline 5.1)

Compilation
~~~~~~~~~~~

- invoke `autogen.sh`: you should end up with a `configure` script.
- then perform the usual drill: `configure`, then `make`.

Legalese
~~~~~~~~
Copyright (c) 2007, Sylvain Fourmanoit <syfou@users.berlios.de>
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.
    
* The names of its contributors may not be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
------------------------------------------------------------------------------*/
/* Headers */
#include <stdlib.h>
#include <string.h>
#include <dirent.h>		/* scandir()  */

#include <sys/types.h> 		
#include <sys/stat.h>		/* open()     */
#include <sys/wait.h>		/* wait_pid() */
#include <fcntl.h>

#include <unistd.h>

#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>

/*----------------------------------------------------------------------------*/
/* Escape expressions
 */
#ifdef __MVS__
#define ESCAPE_C "\047"
#else
#define ESCAPE_C "\033"
#endif
#define ESCAPE(s) ESCAPE_C s

/* Terminal management: see term_setsize 
 */
enum WHMODE { DEFAULT_MODE, TABBED_MODE, RUN_MODE };
#define WIDTH  wh[whmode*2]
#define HEIGHT wh[whmode*2+1]

/*----------------------------------------------------------------------------*/
/* Global states 
 */
char * fname;		   /* Used internally by readline completion hooks   */
int fnamel;

int wh[6] =                /* Used by term_setsize()                         */
  {-1, -1, -1, -1, -1, -1}, 
  whmode=0, only_grow = 0;

char * cmd = NULL;         /* Global because the rl_pre_input_hook needs it  */
char ** expand = NULL;     /* Used for alternate realine completion          */

/*----------------------------------------------------------------------------*/
/* Various helper functions
 */
char * 
ansiprompt(char * prompt, char * ansi)
{
  char * out = NULL;
  if (prompt && ansi && (out = malloc(strlen(prompt) + strlen(ansi) + 11)))
    sprintf(out, 
	    "%c" ESCAPE_C "%s%c%s%c" ESCAPE_C "[00m%c", 
	    RL_PROMPT_START_IGNORE, ansi, RL_PROMPT_END_IGNORE, 
	    prompt,
	    RL_PROMPT_START_IGNORE, RL_PROMPT_END_IGNORE);
  
  return (out)?out:((prompt)?strdup(prompt):NULL);
}

/* Generate ansi escape sequence to resize, xterm-emul style (see resize.c from
   xterm)
 */
void 
term_setsize(int mode)
{
  static int w = -1, h = -1;
  whmode = mode; 

  if (only_grow) {
#define MAX(x,y) (((x)>=(y))?(x):(y))
    w = MAX(w, WIDTH); h = MAX(h, HEIGHT);
#undef MAX
  } else {
    w = WIDTH; h = HEIGHT;
  }
  if (w > 0 && h > 0) {
    printf(ESCAPE("[8;%d;%dt"), h, w);
    fflush(stdout);
  }
}

/* Split a string using separator
 */
char ** 
splitstr(char * str, char sep) 
{
  int i, j, n, err;
  char ** l = NULL, ** tmp;

  if (str) {
    /* Split str in one pass */
    for (err=i=j=n=0; str[j] && !err; ++j)
      if (str[j] == sep || !str[j+1]) {
	if (!str[j+1]) ++j;
	if ((i<j-1)) {
	  if ((tmp = realloc(l, sizeof(char*)*++n)) &&
	      (l = tmp) &&
	      (l[n-1] = malloc(sizeof(char)*(j-i+1)))) {
	    strncpy(l[n-1], &str[i], j-i);
	    l[n-1][j-i]=0;
	  } else
	    err = 1;
	}
	i = j + 1;
	if (!str[j]) --j;
      }
    /* Put a NULL sentinel at the end */
    if (!err) {
      if ((tmp = realloc(l, sizeof(char*)*++n)) &&
	  (l = tmp))
	l[n-1] = NULL;
      else
	err = 1;
    }
    /* Avoid leaks in case of error */
    if (err && l) {
      while(--n)
	free(l[n]);
      free(l);
      l = NULL;
    }
  }
  return l;
}

/* Split $PATH
 */
char ** splitpath(void) { return splitstr(getenv("PATH"), ':'); }

/* Accessory function to match partial name (see scanpath)
 */
int 
name_filter(const struct dirent * e) 
{
  return (e->d_name[0]!='.') && 
    ((fname)?(strncmp(e->d_name, fname, fnamel)==0):1);
}

/* Recursively call scandir on all components of $PATH
 */
char ** 
scanpath(char * name) 
{
  int i, j, k, err;
  char ** d = NULL, ** l = NULL, ** tmp;
  struct dirent **nl;

  fname  = name;
  fnamel = strlen(fname);

  if ((d=splitpath())) {
    for(i=k=err=0;d[i] && !err;++i) {
      if ((j=scandir(d[i], &nl, name_filter, NULL))>0) {
	if ((tmp = realloc(l, sizeof(char**)*(k+j))) && 
	    (l = tmp)) {
	  while(j--) {
	    if (!(l[k++] = strdup(nl[j]->d_name)))
	      err = 1;
	    free(nl[j]);
	  }
	} else
	  err = 1;
      }
      free(d[i]);
    }
    free(d);

    /* Put a NULL sentinel at the end */
    if (!err) {
      if ((tmp = realloc(l, sizeof(char**)*(++k))) && 
	  (l = tmp))
	l[k-1] = NULL;
      else
	err = 1;
    }
    
    /* Avoid leaks in case of error */
    if (err && l) {
      while(--k)
	free(l[k]);
      free(l);
      l = NULL;
    }
  }
  return l;
}

/*----------------------------------------------------------------------------*/
/* History helper function: build the $HOME/.name.hist filename
 */
char * 
history_name(char *name)
{
  char * home, * out = NULL;
  if (!name) name="xrun";
  
  if ((home = getenv("HOME")) &&
      (out = malloc(strlen(home) + strlen(name) + 11)))
    sprintf(out, "%s/.%s.history", home, name);
  return out;
}

/*----------------------------------------------------------------------------*/
/* Readline hooks: completion and line initialization
 */
char * 
alternate_generator(const char *text, int state)
{
  static int i, n;
  if (!state) { 
    i=0;
    n=strlen(text);
  }
  
  while(expand[i++]) {
    if (strncmp(text, expand[i-1], n)==0)
      return strdup(expand[i-1]);
  }
  return NULL;
}

char *
executable_generator(const char *text, int state)
{
  static int i;
  static char ** l;
  
  if (!state) {
    i = 0;
    l = scanpath((char*)text);
  }

  if (l && l[i])
    return l[i++];
  else {
    free(l);
    return NULL;
  }
}

char **
executable_completion(const char * text, int start, int end)
{
  if (start==0) {
    term_setsize(TABBED_MODE);
    if (expand)
      return rl_completion_matches(text, alternate_generator);
    else
      return rl_completion_matches(text, executable_generator);
  }
  return NULL;
}

int 
init_line(void)
{
  if (cmd) {
    rl_replace_line(cmd, 1);
    rl_redisplay();
  }
  return 1;
}

/*----------------------------------------------------------------------------*/
/* Command line parsing helper functions
 */
void 
version(void)
{
  printf("xrun (svn)\n\
Copyright (C) 2007, Sylvain Fourmanoit <syfou@users.berlios.net>\n\
Released under a BSD license.\n");
}

void
usage(void)
{
  printf("Usage: xrun [OPTION] ...\n\
Interactively capture, then run a given command in the default shell.\n\
\n\
  -h		display this help message\n\
  -v            give version information\n\
  -l PROMPT     set default prompt label\n\
  -a ANSI       use the given ansi sequence for the prompt\n\
                (such as '[01;32m', bright green)\n\
  -d GEOM       set default geometry (use WWxHH format)\n\
                of underlying pseudo-terminal using xterm\n\
                escape sequences\n\
  -t GEOM       set geometry on tab expansion (use WWxHH format)\n\
  -r GEOM       set geometry on run (use WWxHH format)\n\
  -g            only grow the terminal geometry: never shrink it\n\
  -c CMD        set default command\n\
  -p PFX        set command prefix\n\
  -s SFX        set command suffix\n\
  -b TIMEOUT    set background timeout (in tenth of second)\n\
                using this option will make xrun execute command\n\
                detached, and bail out considering the command\n\
                was a success on timeout\n\
  -n NAME       set readline name (used in conditional parsing\n\
                of inputrc file, and on selection of the history file),\n\
                default is 'xrun'\n\
  -e            exit on error, default is not to\n\
  -x LIST       set a comma-separated list of keywords for primary\n\
                expantion. By default, xrun use every file in $PATH\n\
\n");
}

int
parse_geometry(char * name, char * geom, int pos)
{
  whmode = pos;
  if (sscanf(geom, "%dx%d", &WIDTH, &HEIGHT)!=2) {
    fprintf(stderr, "%s geometry '%s' could not be parsed\n", name, optarg);
    return 0;
  }
  if (WIDTH<0 || HEIGHT<0) {
    fprintf(stderr, "%s width and height should be positive integers\n", name);
    return 0;
  }
  return 1;
}

int
parse_expantion(char * list)
{
  if (!(expand = splitstr(list, ','))) {
    fprintf(stderr, "could not parse primary expantion list\n");
    return 0;
  }
  return 1;
}

/*----------------------------------------------------------------------------*/
/* Execute the '/bin/sh -c "pfx+cmd+sfx"' command. timeout specify the delay (in
   tenth of a second) we should wait before returning, negative value meaning
   waiting indefinitively -- timed operation implies detaching the child
   from the terminal.

   The function returns 1 on successful command execution (based on sh exit
   code), and 0 on error. When applicable, it assumes the command has succeeded
   if it has not finished on timeout.
*/
int
run(char * pfx, char * cmd, char * sfx, int timeout)
{
  int i, ret = 0;
  char * argv [] = { "/bin/sh", "-c", NULL, NULL };
  pid_t child;

  term_setsize(RUN_MODE);

#define SLEN(s) ((s)?strlen(s):0)
  if ((i = SLEN(pfx) + SLEN(cmd) + SLEN(sfx)) &&
      (argv[2] = malloc(sizeof(char)*(i+1)))) {
#undef SLEN

    /* Build ou the command argument */
    argv[2][0] = 0;
    if (pfx) strcat(argv[2], pfx);
    if (cmd) strcat(argv[2], cmd);
    if (sfx) strcat(argv[2], sfx);

    /* Fork the child */
    switch((child = fork())) {
    case -1:
      perror("fork");
      break;
    case 0:
      /* Child */
      if (timeout >= 0) {
	/* Detach stdin, stdout amd stderr, and reattach to /dev/null */
	for (i=0; i<3; ++i) close(i);
	open("/dev/null", O_RDWR, 0); dup(0); dup(0);
	/* Create new session ID: needed to avoid SIGINT later */
	setsid();
      }
      usleep(100000);           /* Make sure parent's waitpid kicks in */
      execv(argv[0], argv);
      exit(1); 			/* If we got here, something went wrong */
      break;
    default:
      /* Parent */
      if (timeout >= 0) {
	while (timeout-- && waitpid(child, &i, WNOHANG)==0)
	  usleep(100000);
	if (timeout<0) i = 0; 	/* Assumes execution will go fine */
      } else if (waitpid(child, &i, 0)==-1) i = 1;
      ret = (i == 0);
    }
  }
  return ret;
}

/*----------------------------------------------------------------------------*/
/* Entry point
 */
int 
main(int argc, char **argv) 
{
  int c, timeout = -1, exit_on_error = 0;
  char * pfx, * sfx, * prompt, * ansi, * name, * hist;
  
  pfx = sfx = prompt = ansi = name = hist = NULL;

  /* Parsing the command line 
   */
#define OPT(c, arg) case c: arg = optarg
#define OPTB(c, arg) OPT(c, arg); break
#define OPTM(c, name, n) \
  case c: if (!parse_geometry(name, optarg, n)) return 1; break

  while ((c=getopt(argc, argv, "hvl:a:d:t:r:gc:p:s:b:n:ex:"))!= -1)
    switch(c) {
    case 'v': version();                              return 0;
    case 'h': usage();                                return 0;
    OPTB('l', prompt);
    OPTB('a', ansi);
    OPTM('d',"default",0);
    OPTM('t',"tabbed" ,1);
    OPTM('r',"run"    ,2); 
    case 'g': only_grow = 1;                          break;
    OPTB('c', cmd);
    OPTB('p', pfx);
    OPTB('s', sfx);
    case 'b': timeout = atoi(optarg);                 break;
    OPTB('n', name);
    case 'e': exit_on_error = 1;                      break;
    case 'x': if (!parse_expantion(optarg)) return 1; break;
    case '?': usage();                                return 1;
    default : abort();
    }
#undef OPTG
#undef OPTB
#undef OPT

  /* Set up the program
   */
  using_history();
  read_history(hist = history_name(name));

  rl_readline_name = (name)?name:"xrun";
  rl_pre_input_hook = init_line;
  rl_attempted_completion_function = executable_completion;
  
  /* Perform the command capture, and act accordingly
   */
  prompt = ansiprompt(prompt, ansi);
  do {
    term_setsize(DEFAULT_MODE);
    cmd = readline(prompt);
  } while (!exit_on_error && !(c = run(pfx, cmd, sfx, timeout)));

  /* If command went OK, save result back to the history file
   */
  if (c) {
    add_history(cmd); 
    write_history(hist);
  }

  /* Finally free all dynamic structures on exit 
   */
  if (expand) {
    for(c=0;expand[c];++c)
      free(expand[c]);
    free(expand);
  }
  free(hist);
  free(prompt);
  
  return 0L;
}

/*----------------------------------------------------------------------------*/
/* That's all folks! */
