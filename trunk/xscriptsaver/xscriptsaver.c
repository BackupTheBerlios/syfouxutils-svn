/*--- xscriptsaver.c -----------------------------------------------------------
X11 utility for watching keyboard or mouse activity on default X11 display until
a timeout is reached.

------------------------------------------------
Usage: xscriptsaver [-w|--wait] timeout
-------------------------------------------------

It returns when:

1. the timeout (in seconds) is reached without detecting a single keyboard or
  mouse event (return value: 0)

2. an event occured - keyboard was pressed or mouse was moved (return value: 1)

3. an error occured (return value: 2)

If the '--wait' flag is used, (2) shall never occur, and xscriptsaver will loop
internally on events until (1) or (3) happen.

If timeout is set to zero, (1) shell never occur, and xscriptsaver will only
exit on conditions (2) or (3).

xscriptsaver allows writing special purpose screensavers or idle-time manager
for the X Window System. See 'xscriptsaver.sh' for an example.

Requirements
~~~~~~~~~~~~
- A reasonnably POSIX compliant system
- A working C compiler with the basic libraries (this code is C90 plus
  variadic macros... Any gcc version less than ten years old should do)
- X11 libraries and headers

Compilation
~~~~~~~~~~~
Normal build:: `cc -lX11 -o xscriptserver xscriptsever.c`
Debug  build:: `cc -lX11 -DDEBUG -o xscriptserver xscriptsever.c`

You might have to adjust the library or headers path, depending on your system.

Legalese
~~~~~~~~
Copyright (c) 2009, Sylvain Fourmanoit <syfou@users.berlios.de>
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
#include <X11/Xlib.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define HYSTERESIS 10

#define die(...) \
  do { fprintf(stderr, __VA_ARGS__); goto fail; } while(0)


#ifdef DEBUG
#define debug(...) fprintf(stderr, __VA_ARGS__)
#else
#define debug(...) do { } while(0)
#endif

/*----------------------------------------------------------------------------*/
static int
query_pointer(Display * dpy) {
  static int ox = -1, oy = -1;
  unsigned int mask;
  int nx, ny, i, nroots, dummyi, r = 0;
  Window dummyw;

  for(i = 0, nroots = ScreenCount(dpy); 
      i<nroots; 
      ++i)
    if (XQueryPointer(dpy, RootWindow(dpy, i), 
		      &dummyw, &dummyw,
		      &nx, &ny,
		      &dummyi, &dummyi, &mask) == True) {
      if (ox == -1 || (r = mask || 
		       ((abs(nx - ox) + abs(ny - oy)) > HYSTERESIS))) {
	ox = nx;
	oy = ny;
      }
      return r;
    }
  return 0;
}

static int
query_keyboard(Display * dpy) {
  int i;
  char keys[32];
  if (XQueryKeymap(dpy, keys) == True)
    for(i=0; i<32; ++i)
      if (keys[i])
	return 1;
  return 0;
}

/*----------------------------------------------------------------------------*/
int 
main(int argc, char ** argv) {
  int i, timeout, loopflag; // ox, oy, nx, ny;
  Display * dpy;
  
  /* Parse command line */
  if (argc < 2)
    die("Usage: %s [-w|--wait] timeout\n\n", argv[0]);

  for (i = 1, timeout = -1, loopflag = 0; 
       i < argc; 
       ++i)
    if (*argv[i] == '-') {
      if (argv[i][1] == 'w' || argv[i][1] == '-')
	loopflag = 1;
      else {
	if (atoi(argv[i])!=0)
	  die("timeout should be positive\n");
	else
	  die("unknwon flag '%s'\n", argv[i]);
      }
    } else {
      timeout = atoi(argv[i]);
      //if (timeout <= 0   ) die("timeout should be positive\n");
      if (timeout > 99999) die("large value of timeout: overflow?\n");
      debug("timeout: %d\n", timeout);
    }
  if (timeout == -1)
    die("timeout not specified\n");
  if (!timeout && loopflag)
    die("timeout of zero combined with the --wait flag cannot return\n");
  
  /* Prepare for operation */
      if (!(dpy=XOpenDisplay(NULL)))
    die("could not open display\n");

  /* Main loop */
  for(i = timeout * 10; !timeout || i; --i) {
    usleep(100000);
    if (query_pointer(dpy) || query_keyboard(dpy)) {
      if (loopflag)
	i = timeout * 10;
      else 
	break;
    }
  }

  /* Finalize */
  XCloseDisplay(dpy);
  return i != 0;

fail:
  return 2;
}

/*----------------------------------------------------------------------------*/
/* That's all, folks. */
