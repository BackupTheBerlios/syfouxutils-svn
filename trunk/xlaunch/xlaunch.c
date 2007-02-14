/*--- xlaunch.c ----------------------------------------------------------------
X11 utility that modifies the `DISPLAY` variable based on the pointer's
position, before launching a given application. Basically, it makes sure the
default screen in `DISPLAY` is the one the core pointer is on.

---------------------------------------
Usage: xlaunch command [argument 1] ...
---------------------------------------

It was written as a complement to utilities such as
http://hocwp.free.fr/xbindkeys/xbindkeys.html[XBindKeys] when distinct Window
Managers are used on different screens to work around focus issues.

Requirements
~~~~~~~~~~~~
- A reasonnably POSIX compliant system
- A working C compiler with the basic libraries (this code is C90 plus
  variadic macros... Any gcc version less than ten years old should do)
- X11 libraries and headers

Compilation
~~~~~~~~~~~
Normal build:: `cc -lX11 -o xlaunch xlaunch.c`

Debug  build:: `cc -lX11 -DDEBUG -o xlaunch xlaunch.c`

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
#include <unistd.h>

#ifdef DEBUG
#define debug(...) printf(__VA_ARGS__)
#else
#define debug(...)
#endif

#define DPY_NAME_SIZE 128
#define INT_SIZE 20
#define uint unsigned int

/*----------------------------------------------------------------------------*/
int main(int argc, char ** argv) {
  int i, j, n;
  char * dpy_old, dpy_new[DPY_NAME_SIZE], * dpy_tmp, 
    * err = NULL;
  Window dummy;
  Display * dpy = NULL;

  if (strlen(dpy_old = XDisplayName(NULL))) {
    if ((dpy_tmp = strrchr(dpy_old, '.'))) {
      if ((dpy_tmp - dpy_old) < (sizeof(dpy_new)-INT_SIZE-2)) {
	if ((dpy = XOpenDisplay(dpy_old))) {
	  n = ScreenCount(dpy);
	  for(i=0; i<n; ++i)
	    if (XQueryPointer(dpy, RootWindow(dpy, i), &dummy, &dummy, 
			      &j, &j, &j, &j, (uint*)&j) == True) {
	      strncpy(dpy_new, dpy_old, dpy_tmp - dpy_old);
	      if ((j = snprintf(&dpy_new[dpy_tmp - dpy_old], 
				n = (sizeof(dpy_new)-INT_SIZE-2) - 
				    (dpy_tmp - dpy_old),
				".%d", i)) > 0 && j < n) {
		if (setenv("DISPLAY", dpy_new, 1) == 0) {
		  if (argc > 1) {
		    XCloseDisplay(dpy); dpy = NULL;
		    execvp(argv[1], &argv[1]);
		    err = "Could not launch the executable";
		  } else
		    err = "Not program to launch given";
		  debug("New display name: %s\n", dpy_new);
		} else
		  err = "Could not set DISPLAY environment variable";
	      } else
		err = "Could not build the updated display name";
	      break;
	    }
	  if (i>=n)
	    err = "Could not identify screen: stop moving!";
	} else
	  err = "Could not open display";
      } else
	err = "Display name seems awfully long... Bailing out";
    } else
      err = "Could not understand $DISPLAY format";
  } else
    err = "No display name in the environement";

  if (dpy) XCloseDisplay(dpy);
  if (err) fprintf(stderr, "%s\n", err);

  return 1;
}
 
/*----------------------------------------------------------------------------*/

