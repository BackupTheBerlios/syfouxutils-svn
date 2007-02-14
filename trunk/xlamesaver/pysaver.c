/*--- pysaver.c ------------------------------------------------------------000

Basic routines needed to manage a basic, multi-head, monothreaded screensaver
daemon for the X Window System.

See the pure-python xlamesaver.py module for an example of how it can be used.

Legalese
========
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
-----------------------------------------------------------------------------*/
#include <Python.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <signal.h>

#define EVENT_MASK ExposureMask | ButtonPressMask | PointerMotionMask

/*---------------------------------------------------------------------------*/
#ifdef MYDEBUG
#define mydebug(...) printf(__VA_ARGS__)
#else
#define mydebug(...) 
#endif

/*---------------------------------------------------------------------------*/
/* Error handlers global variables
 */
typedef int (*XErrorHandler) (Display *, XErrorEvent *);
static XErrorHandler xhdlr;
static int xstatus;

/* Global variables used to manipulates panes 
 */
static Display * dpy;
static int nroots, hyst, * pstates, * pxy;
static Window * roots, * panes;
static GC * gc;
static Cursor * pcursors;
static PyObject ** pcallbacks, ** pkeywords;

/*---------------------------------------------------------------------------*/
static int 
pysaver_error(Display * dpy , XErrorEvent * xev) {
  xstatus = 1;
#ifndef MYDEBUG
  return 0;
#else
  return xhdlr(dpy, xev);
#endif
}

/*---------------------------------------------------------------------------*/
/* No decoration motif hints for panes 
 */
typedef struct {
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
} MotifWmHints, MwmHints;

#define MWM_HINTS_FUNCTIONS     (1L << 0)
#define MWM_HINTS_DECORATIONS   (1L << 1)

/*---------------------------------------------------------------------------*/
/* Signal blocking function
 */
#define PYSAVER_BLOCK_UNBLOCK(bl, BL) \
static PyObject * \
pysaver_ ## bl (PyObject * self, PyObject * args) { \
  sigset_t mask; \
  if (!(sigemptyset(&mask)==0 && \
	sigaddset(&mask, SIGCHLD) == 0 && \
	sigprocmask(SIG_ ## BL, &mask, NULL)==0)) { \
    PyErr_SetString(PyExc_RuntimeError, "Could not " #bl "signal SIGCHLD"); \
    return NULL; \
  } \
  Py_INCREF(Py_None); \
  return Py_None; \
} 

PYSAVER_BLOCK_UNBLOCK(block, BLOCK)
PYSAVER_BLOCK_UNBLOCK(unblock, UNBLOCK)

#undef PYSAVER_BLOCK_UNBLOCK

/*---------------------------------------------------------------------------*/
/* Helper functions 
 */
static void
pysaver_xcleanup(void) 
{ 
  int i;
  
  if (pcallbacks)
    for (i=0; i<nroots; ++i)
      Py_XDECREF(pcallbacks[i]);

  if (pkeywords)
    for (i=0; i<nroots; ++i)
      Py_XDECREF(pkeywords[i]);

#define XCLEANUP(table) if (table) { PyMem_Free(table); table = NULL; }
  XCLEANUP(roots);
  XCLEANUP(panes);
  XCLEANUP(gc);
  XCLEANUP(pcursors);
  XCLEANUP(pstates);
  XCLEANUP(pcallbacks);
  XCLEANUP(pxy);
#undef XCLEANUP

  XCloseDisplay(dpy);
  dpy = NULL;
  XSetErrorHandler(xhdlr);
  xstatus = 0;
}

static int
pysaver_checkdpy(void)
{
  if (!dpy)
    PyErr_SetString(PyExc_RuntimeError, "not already connected");
  return dpy!=NULL;
}

/*---------------------------------------------------------------------------*/
static char *
pysaver_display_name_low(char * name)
{
  char * dpy_name;
  if (!(dpy_name = 
	XDisplayName((name && strlen(name)>0)?name:NULL)))
    PyErr_SetString(PyExc_RuntimeError, "could not get display name");
  return dpy_name;
}

static PyObject *
pysaver_display_name(PyObject * self, PyObject * args)
{
  char * dpy_name = NULL;
  if (!PyArg_ParseTuple(args, "|s", &dpy_name)) return NULL;
  if (!(dpy_name = pysaver_display_name_low(dpy_name))) return NULL;
  return PyString_FromString(dpy_name);
}

/*---------------------------------------------------------------------------*/
static PyObject * 
pysaver_connect(PyObject * self, PyObject * args, PyObject * kw)
{
  static char * kwlist[] = {"name", "visuals", "hysteresis", NULL};
  int i, n;
  unsigned int d;
  char * name = NULL;
  PyObject * visuals = NULL, * val, * item;
  Screen * s;
  XVisualInfo vinfo, * pinfo;
  Visual * vis;
  XColor col1, col2;
  XSetWindowAttributes attrs;
  Atom hintsAtom = None;
  MotifWmHints hints;
  Pixmap bit;

  hyst = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "|sOi", kwlist, 
				   &name, &visuals, &hyst))
    return NULL;

  if (hyst<0) {
    PyErr_SetString(PyExc_RuntimeError, "hysteresis cannot be negative");
    return NULL;
  }

  if (visuals) {
    if (!PyDict_Check(visuals)) {
      PyErr_SetString(PyExc_RuntimeError, "visuals is not a dictionary");
      return NULL;
    }
  }

  if (dpy) {
    PyErr_SetString(PyExc_RuntimeError, "already connected");
    return NULL;
  }
  
  if (!(name = pysaver_display_name_low(name))) return NULL;

  if (!(dpy = XOpenDisplay(name))) {
    PyErr_SetString(PyExc_RuntimeError, "could not connect to display");
    return NULL;
  }

  /* Set the error handlers */
  xstatus = 0;
  xhdlr = XSetErrorHandler(pysaver_error);
  
  /* Allocates structures */
  nroots = ScreenCount(dpy);

  if (!(roots      = PyMem_New(Window, nroots)) || 
      !(panes      = PyMem_New(Window, nroots)) ||
      !(gc         = PyMem_New(GC, nroots)) ||
      !(pcursors   = PyMem_New(Cursor, nroots)) ||
      !(pstates    = PyMem_New(int, nroots)) ||
      !(pcallbacks = PyMem_New(PyObject*, nroots)) ||
      !(pkeywords  = PyMem_New(PyObject*, nroots)) ||
      !(pxy        = PyMem_New(int, nroots*2))) {
    pysaver_xcleanup();
    PyErr_SetString(PyExc_RuntimeError, "memory allocation problem");
    return NULL;
  }

  /* Create the panes */
  memset((void*)pstates, 0, sizeof(int)*nroots);
  memset((void*)pcallbacks, 0, sizeof(PyObject*)*nroots);
  memset((void*)pkeywords, 0, sizeof(PyObject*)*nroots);
  memset((void*)pxy, 0, sizeof(int)*2*nroots);
  attrs.event_mask = EVENT_MASK;
  memset((void*)&hints, 0, sizeof(MotifWmHints));
  hints.flags = MWM_HINTS_DECORATIONS;
  hints.decorations = 0;
  hintsAtom = XInternAtom (dpy, "_MOTIF_WM_HINTS", False);
  
  for(i=0; i<nroots && !xstatus; ++i) {
    roots[i] = RootWindow(dpy, i);
    mydebug("Screen %d: %dx%d\n", 
	    i,
	    WidthOfScreen(ScreenOfDisplay(dpy, i)),
	    HeightOfScreen(ScreenOfDisplay(dpy, i)));

    s=ScreenOfDisplay(dpy, i);

    vis = NULL;
    if (visuals) {
      if ((item = PyDict_GetItem(visuals, (val = PyInt_FromLong(i))))) {
	if (!PyInt_Check(item)) {
	  Py_DECREF(item);
	  PyErr_SetString(PyExc_RuntimeError, 
			  "Some specified visual ID not integer");
	  xstatus = 1; --i;
	  continue;
	}
	Py_DECREF(item);
	vinfo.visualid = PyInt_AsLong(item);
	if ((pinfo = XGetVisualInfo(dpy, VisualIDMask, &vinfo, &n))) {
	  vis = pinfo[0].visual;
	  d = pinfo[0].depth;
	  XFree(pinfo);
	} else {
	  Py_DECREF(item);
	  PyErr_SetString(PyExc_RuntimeError, 
			  "Could not find visual matching visual ID");
	  
	  xstatus = 1; --i;
	  continue;
	}
      }
      Py_XDECREF(val);
    }

    mydebug("Default visual ID on screen %d: 0x%x\n", 
	    i, (unsigned int)XVisualIDFromVisual(DefaultVisualOfScreen(s)));

    panes[i] = XCreateWindow(dpy, roots[i], 
			     0, 0, 
			     WidthOfScreen(s),
			     HeightOfScreen(s),
			     0, 
			     (vis)?d:DefaultDepthOfScreen(s), 
			     InputOutput, 
			     (vis)?vis:DefaultVisualOfScreen(s),
			     CWEventMask, &attrs);

    XChangeProperty(dpy, panes[i], hintsAtom, hintsAtom,
		    32, PropModeReplace,(unsigned char *) &hints,
		    sizeof (MotifWmHints) / sizeof (long));
    mydebug("Pane %d: 0x%x\n", i, (unsigned int)panes[i]);
    XSync(dpy, False);
  }

  /* Cleanup on panes creation error */
  XSync(dpy, False);
  if (xstatus) {
    for (--i; i>=0; --i)
      XDestroyWindow(dpy, panes[i]);
    pysaver_xcleanup();
    if (!PyErr_Occurred())
      PyErr_SetString(PyExc_RuntimeError, 
		      (vis)?"Window creation problem: check visual":
		      "Window creation problem");
    return NULL;
  }

  /* Create the GCs, one per pane */
  for(i=0; i<nroots && !xstatus; ++i) {
    if ((XAllocNamedColor(dpy, DefaultColormap(dpy, i), 
			  "rgb:00/00/00", &col1, &col2) == 0) &&
	(XAllocColor(dpy, DefaultColormap(dpy, i), &col1) == 0)) {
      
      PyErr_SetString(PyExc_RuntimeError, "X color allocation problem");
      xstatus = 1;
    } else {
      gc[i] = XCreateGC(dpy, panes[i], 0, NULL);
      XSetForeground(dpy, gc[i], col1.pixel);
      mydebug("gc %d, 0x%x\n", i, (unsigned int)gc[i]);

      if (xstatus) 
	PyErr_SetString(PyExc_RuntimeError, "GC initialization problem");
    }
    XSync(dpy, False);
  }
  
  /* Cleanup on color allocation or gc problem */
  if (xstatus) {
    for (--i; i>=0; --i) 
      XFreeGC(dpy, gc[i]);
    for (i=0; i<nroots; ++i)
      XDestroyWindow(dpy, panes[i]);
    pysaver_xcleanup();
    return NULL;
  }

  /* Create the empty cursors, one per pane */
  for(i=0; i<nroots && !xstatus; ++i) {
    if ((bit = XCreatePixmapFromBitmapData(dpy, panes[i],
	         "\000", 1, 1,
		 BlackPixelOfScreen(ScreenOfDisplay(dpy, i)),
		 BlackPixelOfScreen(ScreenOfDisplay(dpy, i)),
		 1))) {
      pcursors[i] = XCreatePixmapCursor(dpy, bit, bit, 
					&col1, &col1,
					0, 0);
      XDefineCursor(dpy, panes[i], pcursors[i]);
      XFreePixmap(dpy, bit);
    } else {
      PyErr_SetString(PyExc_RuntimeError, "Cursor pixmap creation problem");
      xstatus = 1;
    }
    XSync(dpy, False);
  }

  /* Cleanup on cursor creation problem */
  XSync(dpy, False);
  if (xstatus) {
    for (--i; i>=0; --i) 
      XFreeCursor(dpy, pcursors[i]);
    for (i=0; i<nroots; ++i) {
      XDestroyWindow(dpy, panes[i]);
      XFreeGC(dpy, gc[i]);
    }
    return NULL;
  }

  Py_INCREF(Py_None);
  return Py_None;
}

/*---------------------------------------------------------------------------*/
static PyObject *
pysaver_disconnect(PyObject * self, PyObject * args)
{
  int i;

  if (!pysaver_checkdpy()) return NULL;

  for(i=0;i<nroots;++i) {
    XFreeGC(dpy, gc[i]);
    XFreeCursor(dpy, pcursors[i]);
    XDestroyWindow(dpy, panes[i]);
  }
  
  pysaver_xcleanup();
  Py_INCREF(Py_None);
  return Py_None;
}

/*---------------------------------------------------------------------------*/
static PyObject *
pysaver_connected(PyObject * self, PyObject * args)
{
  PyObject * ret = (dpy)?Py_True:Py_False;
  Py_INCREF(ret);
  return ret;
}

/*---------------------------------------------------------------------------*/
static PyObject *
pysaver_screens(PyObject * self, PyObject * args)
{
  return (pysaver_checkdpy())?Py_BuildValue("i", nroots):NULL;
}

static PyObject *
pysaver_activated(PyObject * self, PyObject * args)
{
  int i;

  if (!PyArg_ParseTuple(args, "i", &i)) return NULL;
  if (!pysaver_checkdpy()) return NULL;
  
  if (i<0 || i >= nroots) {
    PyErr_SetString(PyExc_RuntimeError, "screen number out of bound");
    return NULL;
  }
  
  if (pstates[i]) 
    return Py_BuildValue("i", panes[i]);

  Py_INCREF(Py_None);
  return Py_None;
}

static int
pysaver_desactivate_low(int i) 
{
  int status = 1;
  PyObject * r, * a;

  /* This test can seems redundant (see calls in pysaver_pool and
     pysaver_desactivate), but it is not: that's a fine optimization
     in case of desactivation by callbacks.
  */
  if (pstates[i]) {
    mydebug("Unmapping pane %i\n", i);
    XUnmapWindow(dpy, panes[i]);

    /* ...And to run the callback, if one is registered */
    if (pcallbacks[i]) {
      if (pkeywords[i])
	r = PyEval_CallObjectWithKeywords(pcallbacks[i], 
					  a = Py_BuildValue("()"),
					  pkeywords[i]);
      else
	r = PyEval_CallObject(pcallbacks[i], 
			      a = Py_BuildValue("()"));
      
      Py_XDECREF(a);
      Py_XDECREF(r);
      Py_XDECREF(pcallbacks[i]);
      Py_XDECREF(pkeywords[i]);
      pcallbacks[i] = NULL;
      if (!r) status = 0;
    }
    pstates[i] = 0;
  }

  return status && !xstatus;
}

static PyObject *
pysaver_desactivate(PyObject * self, PyObject * args)
{
  int i;
  
  if (!PyArg_ParseTuple(args, "i", &i)) return NULL;
   if (!pysaver_checkdpy()) return NULL;
  
  if (i<0 || i >= nroots) {
    PyErr_SetString(PyExc_RuntimeError, "screen number out of bound");
    return NULL;
  }

  if (pstates[i]) {
    if (!pysaver_desactivate_low(i))
      return NULL;
  }
#ifdef MYDEBUG
  else
    mydebug("Screen %d already desactivated\n", i);
#endif

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject *
pysaver_activate(PyObject * self, PyObject * args, PyObject * kw)
{
  PyObject * cb = NULL;
  int i; 

  if (!PyArg_ParseTuple(args, "i|O", &i, &cb))
    return NULL;

  if (!pysaver_checkdpy()) return NULL;
  
  if (i<0 || i >= nroots) {
    PyErr_SetString(PyExc_RuntimeError, "screen number out of bound");
    return NULL;
  }

  if (cb && !PyCallable_Check(cb)) {
    PyErr_SetString(PyExc_RuntimeError, "callback parameter is not callable");
    return NULL;
  }
  
  if (!pstates[i]) {
    mydebug("Activating screen %d\n", i);
    XMapWindow(dpy, panes[i]);
    XRaiseWindow(dpy, panes[i]);
    Py_XINCREF(cb);
    pstates[i] = 1;
    pcallbacks[i] = cb;
    if (kw) {
      Py_XINCREF(kw);
      pkeywords[i] = kw;
    }
    
  }
#ifdef MYDEBUG
  else 
    mydebug("Screen %d already activated\n", i);
#endif

  return Py_BuildValue("i", panes[i]);
}

/*---------------------------------------------------------------------------*/
static PyObject *
pysaver_pool(PyObject * self, PyObject * args)
{
  char keys[32];
  int i, j, x, y;
  Window dummy;
  XEvent ev;
  
  if (!pysaver_checkdpy()) return NULL;

  xstatus = 0;
  while (!xstatus && XCheckMaskEvent(dpy, EVENT_MASK, &ev)==True) {
#ifdef MYDEBUG
    j = 0;
#endif
    /* Identify the pane */
    for (i=0; i<nroots; ++i)
      if (panes[i] == ev.xany.window)
	break;
    
    /* Discard events on now unmmaped panes */
    if (!pstates[i]) continue;

    /* Act on event */
    switch(ev.type) {
    case Expose:
      XFillRectangle(dpy, panes[i], gc[i], 
		     ev.xexpose.x, ev.xexpose.y, 
		     ev.xexpose.width, ev.xexpose.height);
      mydebug("Expose, pane %d\n", i);
      break;
#ifdef MYDEBUG
#define MYCASE(evt) case evt: if (!j) mydebug("event: " #evt "\n"); j = 1
#else
#define MYCASE(evt) case evt:
#endif
      MYCASE(MotionNotify);
      if ((abs(ev.xmotion.x_root-pxy[2*i]) <= hyst) &&
	  (abs(ev.xmotion.y_root-pxy[2*i+1]) <= hyst))
	break;
      MYCASE(ButtonPress);
      MYCASE(KeyPress);
      MYCASE(KeyRelease);
      /* Time to perform the unmapping... Bail out in case of error */
      if (!pysaver_desactivate_low(i)) return NULL;
      break;
#undef MYCASE
    default:
      mydebug("Ouch! Unhandled event: looks like there is a bug looming...\n");
    }
  }

  /* Query the pointer */
  for (i=0; i<nroots; ++i)
    if (XQueryPointer(dpy, panes[i], 
		      &dummy, &dummy, 
		      &x, &y, 
		      &j, &j, (unsigned int*)&j) == True)
      break;

  /* Query the keyboard */
  if (XQueryKeymap(dpy, keys) == True) {
    for(j=0; j<32; ++j)
      if (keys[j])
	break;
  } else
    j = 32;

  if (xstatus) {
    PyErr_SetString(PyExc_RuntimeError, "An X protocol error occured");
    return NULL;
  }

  if (i<nroots) {
    if (!pstates[i]) {
      pxy[i*2]   = x;
      pxy[i*2+1] = y;
    }
    return Py_BuildValue("(i, ((i, i), O))", 
			 i, x, y, (j>=32)?Py_False:Py_True);
  } else
    return Py_BuildValue("(O, ((i, i), O))", Py_None, 0, 0, Py_False);
}

/*---------------------------------------------------------------------------*/
static PyMethodDef pysaverMethods[] = {
  { "block", pysaver_block, METH_NOARGS,
    "block() => None\n\
Shield from SIGCHLD signal"},
  { "unblock", pysaver_unblock, METH_NOARGS,
    "unblock() => None\n\
Remove shield from SIGCHLD (see block())"},
  { "display_name", pysaver_display_name, METH_VARARGS,
    "display_name(name=None) => dpy_name\n\
Returns the `name' of the display that connect() would attempt to use.\n\
If name is unspecified, the locally implemented lookup mechanism from X\n\
will be used (see documenation to XDisplayName call)"},
  { "connect", (PyCFunction)pysaver_connect, METH_VARARGS | METH_KEYWORDS,
    "connect(name=None, visuals={}, hysteresis=0) => None\n\
Connect to X server named `name' (it connects to default display if no\n\
name given), and initialize the screensaver, using default depth and\n\
visuals ID provided as dictionnary values, keyed by screen number\n\
(if no key/value pair is provided, default visual for each screen is\n\
used). `hysteresis' specify the tolerated pointer slew in pixels for\n\
not desactivating the screen saver. This must be called before any\n\
other call to this module but connected(), display_name(), block()\n\
or unblock()." },
  { "disconnect", pysaver_disconnect, METH_NOARGS,
    "disconnect() => None\n\
Disconnect from X server." },
  { "connected", (PyCFunction)pysaver_connected, METH_NOARGS,
    "connected() => state\n\
Return connection state as a boolean." },
  { "screens", pysaver_screens, METH_NOARGS,
    "screens() => number_of_screens\n\
Return the number of screens on current X display as an integer." },
  { "activate", (PyCFunction)pysaver_activate, METH_VARARGS | METH_KEYWORDS, 
    "activate(screen_num, callback=None, ...) => id\n\
Activate the given screen, while returning the window numeric ID as an\n\
integer. If specified, callback needs to be a callable that will be\n\
invoked right before desactivation: all remaining keywords arguments,\n\
if any, will be passed to the callback." },
  { "desactivate", pysaver_desactivate, METH_VARARGS,
    "desactivate(screen_num) => None\n\
Force desactivation of the given screen: if a callback is registered\n\
(see activate), it is called appropriately. " },
  { "activated", pysaver_activated, METH_VARARGS,
    "activated(screen_num) => id\n\
Return whether or not a given screen is currently activated. If it is,\n\
it returns the window's id on this screen. If it is not, it returns\n\
None." },
  { "pool", pysaver_pool, METH_NOARGS,
    "pool() => (screen, ((x, y), keyboard_active))\n\
First process all X events received since last call, and then\n\
query the X pointer and keyboard. It returns the screen id\n\
(None if the pointer could not be found), the pointer's coordinates\n\
as integers, and the keyboard current activity as boolean.\n\
\n\
IMPORTANT: This should be run in a relatively tight loop (at \n\
least a couple of hertz) once we connect()'ed, the reason being that\n\
the snooping on the pointer and keyboard does not rely on any grab, but\n\
use instant queries (XQueryPointer, XQueryKeymap)." },
  {0}
};

PyMODINIT_FUNC
initpysaver(void)
{
  Py_InitModule3("pysaver", pysaverMethods, "\
xlamesaver daemon core functionnality module.\n\
\n\
pysaver exposes to python the minimum needed to implement a simple \n\
multiheads screensaver daemon for the X Window System. Typical usage\n\
goes like this:\n\
\n\
from pysaver import *\n\
from time import sleep\n\
connect()\n\
try:\n\
   while True:\n\
      print 'Pointer and keyboard info:', pool()\n\
      sleep(.1)\n\
finally:\n\
   disconnect()\n\
\n\
See xlamesaver python source for a more complete example.\n\
");
}
