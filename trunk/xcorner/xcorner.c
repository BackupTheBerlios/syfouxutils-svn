/*--- xcorner.c ----------------------------------------------------------------
X11 utility that automatically runs a `$HOME/.xcornerrc` script whenever the
pointer stays in the lowest right corner of default screen of the display for
two to three seconds.

NOTE: There are other programs doing basically the same thing (such as
http://wiki.catmur.co.uk/Brightside[Brightside]) that are a lot more polished
(and complex!)

Requirements
~~~~~~~~~~~~
- A reasonnably POSIX compliant system
- A working C compiler with the basic libraries (this code is C90 plus
  variadic macros... Any gcc version less than ten years old should do)
- X11 libraries and headers

Compilation
~~~~~~~~~~~
Normal build:: `cc -lX11 -o xcorner xcorner.c`
Debug  build:: `cc -lX11 -DDEBUG -o xcorner xcorner.c`

You might have to adjust the library or headers path, depending on your system.

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
#include <X11/Xlib.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef DEBUG
#define debug(...) printf(__VA_ARGS__)
#else
#define debug(...)
#endif

#define uint unsigned int

/*----------------------------------------------------------------------------*/
char * get_xcornerrc(void) {
  const char * XCORNERRC = "/.xcornerrc";
  char * home, * xcornerrc = NULL;
  if ((home = getenv("HOME")) &&
      (xcornerrc = malloc((strlen(home)) + (strlen(XCORNERRC))))) {
	strcpy(xcornerrc, home);
	strcat(xcornerrc, XCORNERRC);
  }
  return xcornerrc;
}

/*----------------------------------------------------------------------------*/
int main(void) {
  int i, n, x, y;
  uint j, w, h;
  char * xcornerrc, *err = NULL;
  Display * dpy;
  Window root, win, dummy;

  n = 0;
  if ((xcornerrc = get_xcornerrc())) {
    if ((dpy=XOpenDisplay(NULL))) {
      root = DefaultRootWindow(dpy);
      if (XGetGeometry(dpy, root, &win, &i, &i, &w, &h, 
		       (uint*)&i, (uint*)&i) != 0) {
	while(!err) {
	  if (XQueryPointer(dpy, root, &dummy, &dummy, &x, &y, 
			    &i, &i, &j)==True) {
	    if (x==w-1 && y==h-1) {
	      debug("Bottom, right corner!\n");
	      n += 1;
	    } else
	      n = 0;
	    debug("Pointer found on default screen at (%d, %d)\n", x, y);
	  } else
	    n = 0;
	  if (n>2 && fork()==0) {
	      execl(xcornerrc, xcornerrc, NULL);
	      err = "Could not execute $HOME/.xcornerrc";
	  }
	  sleep(1);
	  waitpid(0, &i, WNOHANG);
	}
      } else
	err = "Could not get root window geometry";
      XCloseDisplay(dpy);
    } else
      err = "Could not open display";
  } else
    err = "Cound not resolve $HOME/.xcornerrc";

  if (err)
    fprintf(stderr, "%s\n", err);
  return 1;
}

/*----------------------------------------------------------------------------*/
