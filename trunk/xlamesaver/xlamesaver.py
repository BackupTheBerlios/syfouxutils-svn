'''
Scriptable replacement to the XScreenSaver daemon for special purpose
environments.

Application logic is fully contained in this module.

Disclaimer
==========
XScreenSaver is a mature (> 14 years!) piece of code that has been
thoroughly tested and ported to a variety of systems and ran by
thousands of individuals. BY ALL MEANS, you should stick by it if you
are satisfied with what it provides.

xlamesaver is a considerably shorter and simpler program that aims at
providing the missing functionalities I wanted and were unlikely to
ever make it into the former package.

Problem
=======
I use multi-heads X11 displays (three, four or even five heads) where
the normal operational mode of XScreensaver is just not what I need:

- having all the hacks starting or stopping at the same pointer and
  keyboard events is just not right. I want unused screens to go to
  sleep while the others keep being available. Conversely, having all
  the screens waking up on a single pointer move is often
  counter-productive to me.
  
- The standard mechanism for selecting hacks based on available
  visuals is pretty inconvenient, especially since correct GL
  detection can be tricky in multiheads environments. What I really
  want is a way to specify hacks to activate on a per-screen basis.

- It gets even worse if ever you have multiple machines with different
  hardware sharing the same $HOME: it gets virtually impossible to keep
  a single $HOME/.xscreensaver file around and be satisfied.

- I do run beryl on a few screens as well: on these, I do want to run
  special dbus actions and "xwinwraped" hacks, so I need a way to get
  the information I need out of the daemon to a start/stop script
  without getting the "virtual root" in the way.

Solution
========
xlamesaver provides fully decoupled, scriptable operations on a
per-screen basis, without relying on anything but the core X protocol
(and no grabs!).

With it, you can keep screensavers running on idle screens while the
ones you use stay available, customize behavior on a per-screen basis
(support for XScreenSaver hacks provided), or even run custom scripts
on events the deamon monitors (screen X has been inactive for n
seconds, screen X out of inactivity, etc.).

Limitations
===========
- xlamesaver is only a daemon to monitor and act upon pointer
  movements and command line actions driven by the user: it is a
  replacement for programs such as the XScreenSaver *daemon*. You do
  need external binary or scripts (such as the wonderful XScreenSaver
  hacks or rss-glx) if you want anything sexier than blank screens to
  stare at!
  
- The fancy (and clunky) visual-driven hacks selection from
  XScreenSaver has been completely dropped: the virtual roots the
  hacks are run on are just using default visuals... Set them right!

- The features I did not need were simply not implemented. If you need
  screen locking, dpms support and such, you will have to script it
  yourself.
  
Legalese
========
Copyright (c) 2007, Sylvain Fourmanoit <syfou@users.berlios.de>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
    
* The names of its contributors may not be used to endorse or promote
  products derived from this software without specific prior written
  permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
'''
#-------------------------------------------------------------------------------
# Modules
#
from __future__ import with_statement
from contextlib import closing

import sys, traceback, logging
import os, os.path, time, re
import subprocess, signal
import asyncore, asynchat, socket
import random, textwrap, pprint, optparse
import pysaver

#-------------------------------------------------------------------------------
# Signal protection
#
class Monitor:
  """
  Define a protected code region by shielding it from
  SIGCHLD signal interruption
  """
  _count = 0	# We do refcounting to allow recursive calls

  def __call__(self, func, *args, **kw):
    self._count += 1
    pysaver.block()
    try:
      ret = func(*args, **kw)
    finally:
      self._count -= 1
      if self._count == 0: pysaver.unblock()
    return ret

_protect = Monitor()	# Monitor being basically a singleton, let's create
		        # an instance right away.

#-------------------------------------------------------------------------------
class XLameSaverPrefs(dict):
  """xlamesaver preference file as a dictionary"""
  def __init__(self, path = '%(HOME)s/.xlamesaver', **kw):
    try:
      path = path % os.environ
  
      # Create configuration file if it does not exist
      if not os.path.isfile(path):
        logging.info('Writing preferences to %s',
                     path)
        with file(path, 'w') as f:
          f.write(textwrap.dedent('''
          # XLameSaver preferences file
          # 
          # Have a look at the HOWTO for an overview:
          #
          # http://syfouxutils.berlios.de/xlamesaver/
          #
          from xlamesaver import BlankEvent

          # Force the use of specific visuals: use this
          # if you need to use something different than what is
          # the defaults for your server. Here, visual id 0x21
          # is forced on screen 0:
          #
          # visuals = {0: 0x21}

          # Niceness increment (see `man 2 nice`)
          #
          nice = 5
          
          # Tolerated pointer hysteresis (in pixels)
          #
          hysteresis = 10

          # Mode at startup time
          #
          mode = "default"
          
          # Extremely simple configuration: all screens will be
          # blanked after 60 seconds.
          #
          events = [ BlankEvent(time=60, dbglabel="blank all") ]
          ''').strip() + '\n')
        
      # Then load it
      config = {}
      with file(path) as f:
        exec f in config
        del config['__builtins__']
      dict.__init__(self, config)

      # Add the forced parameters
      self.update(kw)
        
      # And make sure some defaults are set
      for k, v in (('display', ''), ('visuals', {}), ('hysteresis', 10),
                   ('nice', 5), ('mode', 'default'), ('events', [])):
        if not self.has_key(k):
          self[k] = v
          
    except:
      logging.error('Could not load preferences file')
      raise

#-------------------------------------------------------------------------------
class XScreenSaverPrefs(dict):
  """XScreenSaver preference file as a dictionary"""
  
  def __init__(self, path = '%(HOME)s/.xscreensaver'):
    dict.__init__(self)
    self.keyvals = re.compile('^(\w+):\s*(.*)$')
    self.empty   = re.compile('^\s*$')
    self.hack    = re.compile('^(-?)\s*(\w+:)?\s*("[^"]*")?(.*)$')
    self.time    = re.compile('^\d+:\d+:\d+$')
    self.number  = re.compile('^-?\d+$')
    self.true    = re.compile('^True$', re.IGNORECASE)
    self.false   = re.compile('^False$', re.IGNORECASE)
    self.load(path)

  def load(self, path = '%(HOME)s/.xscreensaver'):
    """Reload preferences from file"""
    def while_not(iterable, cond):
      for item in iterable:
        if not cond(item):
          yield item
        else:
          break
    def parse_hack(hack):
        m = self.hack.match(hack)
        if m:
          return (m.group(4).strip().split()[0],
                  {'active': len(m.group(1))==0,
                   'visual': m.group(2)[:-1] \
                   if not m.group(2) is None else None,
                   'desc'  : m.group(3),
                   'cmd'   : ' '.join(m.group(4).strip().split())})
        else:
          return None
    def parse_val(val):
      val = val.strip()
      if self.time.match(val):
        return sum(
          [(int(item) * 60**(2-i))
           for i, item in enumerate(val.split(':'))])
      elif self.number.match(val):
        return int(val)
      elif self.true.match(val):
        return True
      elif self.false.match(val):
        return False
      else:
        return val

    self.clear()
    feed = self._spoonfeed(path)
    for line in feed:
      m = self.keyvals.match(line)
      if m:
        key, val = m.groups()
        if m.group(1) == 'programs':
          val = dict([parse_hack(hack)
                      for hack in ([val] +
                                   [hack
                                    for hack in while_not(feed,
                                                          self.empty.match)])])
        else:
          val = parse_val(val)
        self[key] = val
    
  def _spoonfeed(self, path):
    for line in \
       file(path % os.environ).read().decode('string_escape').split('\n'):
      yield line

#-------------------------------------------------------------------------------
class Event:
  """Base event class

  It can be instantiated directly, but it does nothing: any
  combination of start(), stop() and tic() need to be overloaded.
  """

  display = None	# Display name: set in Events manager
  
  def __init__(self, screens=None, time=60, modes=['default'],
               dbglabel=None, **kw):
    """
    screens, time and modes parameters are there to specify the
    conditions under which the event should be triggered; dbglabel in
    used to generate more expressive object representation when
    debugging
    """
    self._state      = 0
    self._allscreens = screens is None
    self._next_tic   = 0
    self.screens     = [] if self._allscreens else screens
    self.time        = time
    self.modes       = modes
    self.dbglabel    = dbglabel

    self.config(**kw)

  def config(self, **kw):
    """
    Configuration hook

    It is a lot less verbose and bug prone to add such a hook to
    handle remaining initializer arguments than overloading it.
    """
    if len(kw) > 0:
      raise RuntimeError('Unhandled initializer argument(s): %s' %
                         ', '.join(kw.keys()))

  def __repr__(self):
    return '<evt %s: screens %s%s%s>' % (('"%s"' % self.dbglabel)
                                         if self.dbglabel else '',
                                         self.screens,
                                         ', time %s' % self.time
                                         if self.time is not None else
                                         ' (compound)',
                                         ', modes %s' % self.modes
                                         if self.modes is not None else '')

  def display_name(self, screen):
    """Mangle display name to specify new default screen"""
    return '.'.join(self.display.split('.')[:-1] + ['%d' % screen])
  
  def refresh(self, srange=[]):
    """
    Expand screens covered by this event to all the available screens
    if specific screens are not specified at initialization.

    This has been done this way so that we can instanciate events
    orthogonally from low-level pysaver connections; this also means
    we need to call Event.refresh() once each time such connection
    change, before invoking Event.match()... This is taken care of by
    the Events manager (see Events class).
    """
    self.srange = srange
    if self._allscreens:
      self.screens = srange
    
  def running(self):
    return bool(self._state % 2)
    
  def match(self, activity, mode):
    """Check if state switch is needed

    Basically, it discards events related to missing screens, then it
    checks for possible time-based state transitions if the event is
    part of current mode, or ask for state switch if the event is
    running while current mode has switched to a mode when this event
    should not be.
    """
    def thres(a, b, gt): return a > b if gt else a <= b

    if len(set(self.screens).difference(self.srange)) == 0:
      if mode in self.modes:
        return thres(min([activity[screen] for screen in self.screens]),
                     self.time, not self.running())
      else:
        return self.running()      
    return False

  def toggle(self):
    """
    Switch state: call self.start() and self.stop() appropriately:

    Note: *Never* switch state from your own state handling routines;
    state changing is the sole prerogative of the events manager
    (see Events class).
    """
    self._state += 1;
    if self.running():
      _protect(self._start)
    else:
      _protect(self._stop)

  def _start(self):
    logging.debug('Start %s' % repr(self))
    self.start()
    self.run_tic()

  def _stop(self):
    logging.debug('Stop %s' % repr(self))
    self.stop()
    self.run_tic()

  def run_tic(self):
    """
    Generate (and call) tic event as appropriate
    """
    if self.running():
      if self._next_tic is not None and time.time() >= self._next_tic:
        try:
          self._next_tic = time.time() + _protect(self.tic)
        except TypeError:
          self._next_tic = None
    else:
      self._next_tic = 0

    return self._next_tic

  def start(self):
    """Fire up event"""
    pass
  
  def stop(self):
    """Stop event"""
    pass
             
  def tic(self):
    """Tic event"""
    pass

#-------------------------------------------------------------------------------
class BlankEvent(Event):
  """
  Blank screens event

  Blank one or many screens
  """
  def start(self):
    for screen in self.screens:
      pysaver.activate(screen)

  def stop(self):
    for screen in self.screens:
      pysaver.desactivate(screen)

#-------------------------------------------------------------------------------
class ExternalProcessEvent(Event):
  """Base class for events managing an external process"""
  
  class Child(subprocess.Popen):
    """Single process manager with integrated zombie collection"""
    def __init__(self, cmd):
      self._killflag = False
      self._install_childhdlr()
      subprocess.Popen.__init__(self,
                                cmd.split() if isinstance(cmd, str) else cmd)

    def _install_childhdlr(self):
      if (signal.getsignal(signal.SIGCHLD) in
          (signal.SIG_IGN, signal.SIG_DFL, None)):
        signal.signal(signal.SIGCHLD, self._childhdlr)

    def _childhdlr(self, signum, frame):
      try:
        _protect(os.waitpid, 0, os.WNOHANG)
      except OSError: pass
      self._install_childhdlr()

    def kill(self): _protect(self._kill)
    
    def _kill(self):
      if self.poll() is None:
        try:
          os.kill(self.pid,
                  signal.SIGKILL if self._killflag else signal.SIGQUIT)
        except OSError: pass
      self._killflag = True
    
  def config(self, cycle=None, activate=True):
    self.cycle    = cycle
    self.activate = activate
    self.win      = {}
    self.children = {}

  def kill_child(self, screen):
    if self.children.has_key(screen): self.children[screen].kill()

  def start(self):
    if self.activate:
      for screen in self.screens:
        self.win[screen] = pysaver.activate(screen, self.kill_child,
                                            screen=screen)

  def stop(self):
    for screen in self.screens:
      if self.activate: pysaver.desactivate(screen)
      self.kill_child(screen)
    
  def tic(self):
    for screen in self.screens:
      self.kill_child(screen)
      os.environ['DISPLAY'] = self.display_name(screen)
      self.children[screen] = self.Child(
        self.cmd(screen) % {'window':
                            self.win[screen]
                            if self.win.has_key(screen) else 0,
                            'screen': screen})
    return self.cycle

  def cmd(self, screen):
    """Returns the complete command to run on screen, as a string"""
    raise RuntimeError('%s is virtual' % self.__class__)

#-------------------------------------------------------------------------------
class ScriptEvent(ExternalProcessEvent):
  """
  Call arbitrary program using command line specified by `command'.

  The called command can either be long running or not, its your call:
  it will be killed as appropriate on stop or on cycling, but it is
  your duty to kill other child process you spawned yourself. Do not
  hesitate to reuse ExternalProcessEvent.Child, as it comes with an
  integrated SIGCHLD handler and correct sigblock, not available from
  any stock Python module.

  Please note that a simple interpolation mechanism exists for you to
  reuse: you can use '0x%(window)x' and '%(screen)d' in the string
  returned by self.cmd() if you want to substitute activated window id
  (hexadecimal) and screen number respectively in the final command
  line.
  """
  def config(self, cycle=None, activate=True, command=None):
    ExternalProcessEvent.config(self, cycle, activate)
    self.command = command
    if command is None:
      raise TypeError('command parameter unspecified in %s' % self.__class__)

  def cmd(self, screen):
    return self.command

#-------------------------------------------------------------------------------
class XScreenSaverEvent(ExternalProcessEvent):
  """
  Call random XScreenSaver hacks on specified screens

  Using default parameters, this event replicates the XScreenSaver
  daemon behavior in random mode, with no cycling, no dpms nor screen
  locking management.

  The per-hack parameters you set up using xscreensaver-demo will be
  used (xlamesaver parses XScreenSaver preferences file); you can also
  specify hacks_criteria to change the way the hacks are selected on a
  per-screen basis... Here is an example:

  hacks_criteria = {0: {'visuals': ['GL']},
                    1: {'activity': [True, False],
                        'exclude' : ['phosphor']}}

  This say that only the active hacks with a GL visual should be
  selected on screen 0, while both active and inactive hacks but the
  phosphor hack should be selected on screen 1.

  The valid keys, values pairs that can be used in hacks_criteria
  dictionary values are 'visuals' (a list of valid visuals),
  'activity' (a list of valid boolean states), 'names' (a list of
  hacks names), and 'exclude' (a list of hacks names).

  Other events parameter are 'cycle' (the delay in seconds before
  switching hacks), 'hacks_path' (the path name to the xcreensaver
  hacks), 'xscreensaver_config' (the path to the XScreenSaver
  preferences file to use), and 'sync' (if True, the same hacks will
  be run on all the screens the event is run on: it is the same than
  the random-same mode from XScreenSaver).
  """
  def config(self, cycle=None,
             hacks_path = '/usr/lib/misc/xscreensaver',
             xscreensaver_config = '%(HOME)s/.xscreensaver', 
             hacks_criteria = {},
             sync = False):
    ExternalProcessEvent.config(self, cycle, activate=True)
  
    self.xconf  = XScreenSaverPrefs(xscreensaver_config)
    self.crits  = hacks_criteria
    self.path   = hacks_path
    self.sync   = sync
    self.cur    = {}
    self.common = None

  def refresh(self, srange=[]):
    def crit(screen):
      return self.crits[screen] if self.crits.has_key(screen) else {}
    
    def iterhacks(activity = [True], visuals=[], names=[], exclude=[]):
      for k, v in self.xconf['programs'].iteritems():
        if ((v['active'] in activity) and
            (len(visuals)==0 or v['visual'] in visuals) and
            (not k in exclude) and 
            (len(names)==0 or (k in names))):
          yield k
    
    Event.refresh(self, srange)

    # List the hacks to use
    #
    self.hacks = dict([(screen, list(iterhacks(**crit(screen))))
                       for screen in self.screens])

    # Check common hacks, as needed
    if self.sync:
      for h in self.hacks.itervalues():
        if self.common is None:
          self.common = set(h)
        else:
          self.common.intersection_update(h)
      if self.common is None or len(self.common)==0:
        raise RuntimeError('no hacks common to all screens based on criteria')
      else:
        self.common = list(self.common)
        
    # Randomize the order in place, set current position
    if not self.sync:
      for screen in self.screens:
        if len(self.hacks[screen])==0:
          raise RuntimeError('no suitable hack found for screen %d' % screen)
        random.shuffle(self.hacks[screen])
        self.cur[screen] = 0
    else:
      random.shuffle(self.common)
      self.cur = 0

  def cmd(self, screen):
    def index(t, i): return t[i % len(t)]
    if self.sync:
      hack = index(self.common, self.cur)
      if screen == self.screens[-1]: self.cur += 1
    else:
      hack = index(self.hacks[screen], self.cur[screen])
      self.cur[screen] += 1
      
    logging.info('Running hack "%s" on screen %d' % (hack, screen))
    return os.path.join(self.path, self.xconf['programs'][hack]['cmd']) + \
           ' -window-id 0x%(window)x'
    
#-------------------------------------------------------------------------------
class CompoundEvent(Event, list):
  """
  Compound event

  It gives a way to force various, unrelated events using
  non-overlapping screens to react synchronously to pointer and
  keyboard: see this as an extension to the Events handler that can
  make partitions of your screens behave in the same way XScreenSaver
  would, event if you do not wish to run a single XscreenSaverEvent on
  a given partition.

  All you have to do is to pass sub-events to the initializer as an
  iterable using keyword 'subevents'. Be aware that the 'mode' and
  'time' properties of the subevents will be completely ignored.
  """
  class StartStop:
    def __init__(self, mode, subevents):
      self.mode = mode
      self.subevents = subevents
      
    def __call__(self):
      def nop(v, op): return v if op else not v
      for evt in self.subevents:
        if nop(evt.running(), self.mode):
          evt.toggle()

  def __hash__(self):
    # Needed because lists are not hashable
    return id(self)
  
  def config(self, subevents=[]):
    list.__init__(self, subevents)

    for meth in ('start', 'stop'):
      setattr(self, meth, self.StartStop(meth == 'stop', self))

  def refresh(self, srange=[]):
    Event.refresh(self, srange)

    # All right, so this is clearly unpythonic, but let's add a few
    # checks to avoid headaches to users. ;-)
    def flatten(events):
        for evt in events:
          for screen in evt.screens:
            yield screen
    try:
      # Check for compound events
      for evt in self:
        evt.refresh(self.screens)
        evt.time  = None
        evt.modes = None
        
        if isinstance(evt, CompoundEvent):
          raise TypeError('CompoundEvent cannot be a subevent of itself')
          
      # Check for out-of-bound screen range
      s = set(flatten(self)).difference(self.screens)
      if len(s) > 0:
        raise ValueError('Screen(s) %s out of CompoundEvent %s' %
                         (', '.join([str(i) for i in s]), repr(self)))

      # Check for overlapping events
      s = list(sorted(flatten(self)))
      for i, j in zip(s, s[1:]):
        if i == j:
          raise ValueError(
            'Screen %d is used by multiple subevents in CompoundEvent %s' %
            (i, repr(self)))
    except:
      logging.error('Invalid events configuration')
      raise

  def tic(self):
    try:
      return min([evt.run_tic()
                  for evt in self if evt.run_tic() is not None]) - time.time()
    except ValueError:
      return None

#-------------------------------------------------------------------------------
class ManagerEvent(Event):
  """
  Base class for manager events

  Manager events are events having direct access to the current events
  manager through their self.events property.

  The number of ways you can shoot yourself in the foot using this is
  just astounding, but basically manipulating most states from the
  events manager is fine from the any of the start(), stop() and tic()
  methods... Use it with care.
  """
  def config(self):
    self.events = None

#-------------------------------------------------------------------------------
class StateChangeEvent(ManagerEvent):
  """
  Daemon state changing event

  This event makes possible to trigger the same changes in the Daemon
  states than what is possible manually from the command line.

  You can change the mode to `to_mode', as well as forcing timer reset
  on provided `reset_screens' (sequence of screen number as integers)
  to the specified `reset_offsets' (dictionary of time offsets in
  seconds as integers indexed by screen number), if given (offset of
  zero by default).
  """
  def config(self, to_mode=None, reset_screens = [], reset_offsets={}):
    ManagerEvent.config(self)
    
    self.to_mode       = to_mode
    self.reset_screens = reset_screens
    self.reset_offsets = reset_offsets

  def start(self):
    if not self.to_mode is None:
      logging.debug('Switching mode by event (%s => %s)' %
                    (self.events.modes[-1], self.to_mode))
      self.events.modes[-1] = self.to_mode
    for screen in self.reset_screens:
      offset = self.reset_offsets[screen] if \
               self.reset_offsets.has_key(screen) else 0
      logging.debug('Resetting time by event to %d offset on screen %d' %
                    (offset, screen))
      self.events.states.reset(screen, offset)
      
#-------------------------------------------------------------------------------
class Events(list):
  """
  That's the event manager: it fires up and down events based on screens
  activity.
  """
  class States(list):
    """Low level state on each screen"""
    def __init__(self, hysteresis=0):
      list.__init__(self, [time.time()] * pysaver.screens())
      self.hyst = hysteresis;
      self.x    = -self.hyst
      self.y    = -self.hyst

    def reset(self, screen, offset=0):
      """Reset timing information on a given screen"""
      self[screen] = time.time() - offset
      
    def pool(self):
      """Refresh the information: should be called periodically"""
      screen, ((x, y), keyboard) = pysaver.pool()
      if not screen is None:
        if (keyboard or
            abs(x-self.x) > self.hyst or
            abs(y-self.y) > self.hyst):
          self.reset(screen); self.x = x; self.y = y
      return self

    def activity(self):
      """Translate the state in function of current time"""
      t = time.time()
      return [t - item for item in self]

  def __init__(self, prefs):
    list.__init__(self)

    self.prefs    = prefs
    self.states   = self.States(prefs['hysteresis'])
    self.modes    = [prefs['mode']]
    self.screens  = [None] * pysaver.screens()
    
    Event.display = prefs['display']
    for evt in prefs['events']: self.append(evt)

  def _check(self, evt):
    """
    Perform a few tests and updates to event internal structure, just
    before it gets managed by the events handler
    """
    if not isinstance(evt, Event):
      raise TypeError('%s is not an Event' % repr(evt))
    if isinstance(evt, ManagerEvent): evt.events = self
    evt.refresh(range(pysaver.screens()))
    return evt
  
  def append(self, evt):
    list.append(self, self._check(evt))

  def __setitem__(self, k, v):
    list.__setitem__(self, k, self._check(v))
    
  def pool(self):
    """
    This is the method responsible for scheduling the various events:
    call it in a relatively tight loop.

    The implemented behavior is straightforward:

    - There is at most one event running on a given screen at any
      given time.
      
    - An active event is always desactivated as soon as its
      desactivation condition is reached (see `Event.match()').

    - An unactive event is activated if it reaches its activation
      condition (see `Event.match()'), and if it doesn't conflict on
      at least one screen with another already active event of greater
      or equal seniority, where the seniority is determined by the time
      threshold that has to be reached to activate a given event: the
      longer it takes, the more 'senior' an event is.

    It was written to be pretty resilient to states manipulation by
    ManagerEvents instances, but read the code itself if you ever need
    to write one of these.
    """
    def etime(evt): return evt.time

    # Update the activity data
    activity = self.states.pool().activity()

    # Classify the events needing reactions
    matches   = [evt for evt in self if evt.match(activity, self.modes[-1])]
    inactives = [evt for evt in matches if evt.running()]
    actives   = [evt for evt in matches if not evt.running()]
    
    # Clear all inactive events
    for inactive in inactives:
      for screen in inactive.screens:
        self.screens[screen] = None
      inactive.toggle()

    # Activate what is activable from wannabe active events: starting with the
    # ones most likely to be activated.
    for active in reversed(sorted(actives, key=etime)):
      conflicts = set([self.screens[screen]
                       for screen in active.screens])
      conflicts.discard(None)

      # Bail out if this active event does not have the required seniority
      if (len(conflicts)> 0 and
          max(conflicts, key=etime).time >= active.time):
        continue

      # So we are fine: stop all conflicting events now
      for conflict in conflicts:
        for screen in conflict.screens:
          self.screens[screen] = None
        conflict.toggle()

      # Fire up this active event
      for screen in active.screens:
        self.screens[screen] = active

      active.toggle()

    # And finally make sure to trigger tic events
    for evt in self: evt.run_tic()

#-------------------------------------------------------------------------------
# Various context managers
#
class XConnect:
  """Context manager handling connection to X Server"""
  def __init__(self, prefs):
    self.prefs = prefs
    
  def __enter__(self):
    try:
      pysaver.connect(self.prefs['display'],
                      self.prefs['visuals'], self.prefs['hysteresis'])
    except:
      logging.error(str(sys.exc_info()[1]))
      return False
    else:
      return True
            
  def __exit__(self, t, v, tb):
    logging.info('Exiting')
    if pysaver.connected():
      pysaver.disconnect()
    return t is None or t is KeyboardInterrupt

class DisplayName:
  """Context manager for handling display name determination"""
  def __init__(self, name):
    self.name = name
    
  def __enter__(self):
    try:
      name = pysaver.display_name(self.name)
    except RuntimeError, e:
      logging.error(str(e))
      return None
    else:
      return name

  def __exit__(self, t, v, tb):
    return t is None

#-------------------------------------------------------------------------------
# Interprocess communication utilities
#
def _unix_addr(dpy_name):
  """Compute a unique key for (user, display) combinations"""
  return '\x00xlamesaver-%d-%s' % (os.getuid(),
                                   '.'.join(dpy_name.split('.')[:-1]))

def send_command(dpy_name, cmd):
  """Send a command to the daemon."""
  logging.info(cmd)
  with closing(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)) as s:
    try:
      s.connect(_unix_addr(dpy_name))
      s.send(cmd + '\n');
      logging.info(s.recv(1024))
    except socket.error, e:
      logging.error(('Could not carry out command (%s): ' + 
                     'is the daemon running on this display?') % e[1])

class CommandServer(asyncore.dispatcher):
  """Process incoming commands from daemon"""
  class CommandChannel(asynchat.async_chat):
    def __init__(self, conn, server):
      asynchat.async_chat.__init__(self, conn=conn)
      self.server = server
      self.set_terminator('\n')
      self.cmd = ''

    def collect_incoming_data(self, data):
      self.cmd += str(data)

    def found_terminator(self):
      """Commands from clients are processed here"""
      def makedefaults(defaults, args):
        return dict([(item[0], item[1]) for item in defaults] +
                    zip([i[0] for i in defaults],
                        [eval('%s(%s)' % (defaults[j][2], k))
                         for j, k in enumerate(args)]))

      try:
        cmd = 'undefined'
        s = self.cmd.split(); cmd = s[0]; args = s[1:]; msg = None

        if cmd == 'reset':
          args = makedefaults([('screen', None, 'int'),
                               ('offset', 0, 'int')], args)
          logging.debug('Resetting time to %d offset on screen %d' %
                        (args['offset'], args['screen']))
          self.server.events.states.reset(args['screen'], args['offset'])
        elif cmd == 'resetall':
          args = makedefaults([('offset', 0, 'int')], args)
          logging.debug('Resetting time to %d offset on all screens' %
                        (args['offset']))
          for screen in xrange(len(self.server.events.states)):
            self.server.events.states.reset(screen, args['offset'])
        elif cmd == 'pop':
          if len(self.server.events.modes) > 1:
            self.server.events.modes.pop()
          else:
            raise RuntimeError('Cannot pop initial mode of operation')
        elif cmd == 'mode':
          args = makedefaults([('mode', 'default', 'str')], args)
          logging.debug('Switching mode (%s => %s)' %
                        (self.server.events.modes[-1], args['mode']))
          self.server.events.modes.append(args['mode'])
        elif cmd == 'info':
          logging.debug('Daemon queried for info')
          msg = pprint.pformat({
            'display': self.server.prefs['display'], 
            'screens': range(pysaver.screens()),
            'modes': self.server.events.modes,
            'active_events:': self.server.events.screens,
            'activity': self.server.events.states.activity()
            })
        elif cmd == 'exit':
          self.server.exit = True
        else:
          raise RuntimeError('Unknown command "%s"' % cmd)
      except RuntimeError, e:
        msg = str(e)
      except:
        msg = 'Error from daemon:\n==================\n' + \
              ''.join(traceback.format_exception(*sys.exc_info()))
        logging.error('Command "%s" triggered an error in daemon' % cmd)
      else:
        if msg is None: msg = 'OK'
      finally:
        self.push(msg)
        self.close_when_done()

  def __init__(self, prefs):
    asyncore.dispatcher.__init__(self)
    self.prefs  = prefs

  def __enter__(self):
    try:
      self.events = Events(self.prefs)
      self.exit = False
      os.nice(self.prefs['nice'])
      self.create_socket(socket.AF_UNIX, socket.SOCK_STREAM)
      self.bind(_unix_addr(self.prefs['display']))
      self.listen(1)
    except socket.error, e:
      logging.error('%s: would a daemon already be running?' % e[1])
      return None
    else:
      logging.info('Daemon started, listening...')
      return self

  def __exit__(self, t, v, tb):
    self.close()
    return t is None

  def handle_accept(self):
    channel, addr = self.accept()
    self.CommandChannel(channel, self)
  
  def handle_connect(self): pass
  def handle_write(self): pass

  def loop(self):
    while not self.exit:
      t = time.time()
      asyncore.loop(count=1)
      self.events.pool()
      time.sleep(max(t + .1 - time.time(), 0))

#-------------------------------------------------------------------------------
# Entry point
#
def cli():
  """
  Entry point for command line use: command line is parsed, and daemon
  or command-line client started as appropriate.
  """
  # First, make sure the umask is properly set: no writing permissions to
  # others: we don't want them talking to our daemon.
  mask = os.umask(0777); os.umask(mask | 022); del mask
  
  # Build and parse the command line...
  p = optparse.OptionParser(usage="""%prog [OPTIONS]
Highly-scriptable screensaver management daemon and command-line controller.

All options up to --verbosity are available to both to the daemon and the
command-line controller. All options below --verbosity are specific to the
controller.""",
                            version="""xlamesaver (subversion)
Copyright (C) 2007 Sylvain Fourmanoit <syfou@users.berlios.net>
This is open source software, distributed under BSD license. There is NO
WARRANTY, to the extent permitted by law.""")
                            
  p.add_option('-d', '--display',
               dest='display', action='store', type='string', default='',
               help='specify the X display to use')
  p.add_option('-c', '--config',
               dest='prefs', action='store', type='string',
               default='%(HOME)s/.xlamesaver',
               help=('specify the preferences file to use ' +
                     '(default $HOME/.xlamesaver)'))
  p.add_option('-v', '--verbosity',
               dest='level', action='store', default=2, type='int',
               help=('set verbosity level: 2 is all messages ' +
                     '(debug, info and errors), 1 is some (info and errors), ' +
                     'and 0 only displays errors. Default is 2'))
  p.add_option('-r', '--reset',
               dest='screen', action='store', default=None, type='int',
               help='force timer reset on given screen')
  p.add_option('-a', '--reset-all',
               dest='all', action='store_true', default=False,
               help='force timer reset on all available screens')
  p.add_option('-m', '--mode',
               dest='mode', action='store', default=None, type='string',
               help='push new mode of operation, as a string')
  p.add_option('-p', '--pop',
               dest='pop', action='store_true', default=False,
               help='pop last mode of operation')
  p.add_option('-o', '--offset',
               dest='time', action='store', default=0, type='int',
               help=('specify offset (in seconds) to use when resetting ' +
                     'screen timers'))
  p.add_option('-i', '--info',
               dest='info', action='store_true', default=False,
               help='query info from daemon')
  p.add_option('-e', '--exit',
               dest='exit', action='store_true', default=False,
               help='ask daemon to exit')
              
  opts, args = p.parse_args()

  # Determine the invocation mode
  #
  daemonic = not (opts.screen is not None or
                  opts.all or opts.info or opts.pop or
                  opts.mode is not None or
                  opts.exit)

  # Set up the logging facility
  #
  logging.basicConfig(level={2:logging.DEBUG,
                             1:logging.INFO,
                             0:logging.ERROR}[
    opts.level if opts.level in range(3) else 2],
                      datefmt='%d-%m %H:%M:%S',
                      format='%(asctime)s %(levelname)s %(message)s'
                      if daemonic else '%(message)s')

  with DisplayName(opts.display) as display:
    if not display is None:      
      # ...then act on it.
      if daemonic:
        # Daemonic invocation
        prefs = XLameSaverPrefs(opts.prefs, display=display)
        with XConnect(prefs) as xstatus:
          if xstatus:
            with CommandServer(prefs) as server:
              if not server is None:
                server.loop()
          else:
            logging.error('Daemon X initialization failed')
      else:
        # Command line invocation
        if opts.screen is not None:
          send_command(display, 'reset %d %d' % (opts.screen, opts.time))
    
        if opts.all:
          send_command(display, 'resetall %d' % (opts.time))

        if opts.pop:
          send_command(display, 'pop')
          
        if opts.mode is not None:
          send_command(display, 'mode "%s"' % (opts.mode))

        if opts.info:
          send_command(display, 'info')
      
        if opts.exit:
          send_command(display, 'exit')

#-------------------------------------------------------------------------------
# Invoke the intery point if called as module
#
if __name__ == '__main__': main()
