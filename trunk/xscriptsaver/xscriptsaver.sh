#! /bin/sh
# 
# Here is an demo of a screensaver script deamon for compiz.
#
# What it does is calling a sequence of effects depending on how long the 
# X11 session has been inactive:
# 
# - After one minute, it ask for a rain effect
# - After two minutes, it desactivates the rain effect, and fire up snow
# - After five, it changes desktop to the left, keeping snow on
# - After ten, it stops everyting, as dpms was told to fire in
#
# In addition, it never starts if an application called lplayer is running for
# the current user, or if the pointer is on the left border of the screen.
#
# Neat, isn't? 
#
# 	-- Sylvain Fourmanoit <syfou@users.berlios.de>
#
#-------------------------------------------------------------------------------
ROOT=`xwininfo -root | gawk '/id:/ { print $4 }'`

#-------------------------------------------------------------------------------
dbus_activate() {
    dbus-send --type=method_call \
	--dest=org.freedesktop.compiz $1 \
	org.freedesktop.compiz.activate \
	string:'root' int32:${ROOT}
}

SNOW=off
toggle_snow() {
    test x$1 = x${SNOW} || \
	dbus_activate /org/freedesktop/compiz/snow/allscreens/toggle_key
    SNOW=$1
}

RAIN=off
toggle_rain() {
    test x$1 = x${RAIN} || \
	dbus_activate /org/freedesktop/compiz/water/allscreens/toggle_rain_key
    RAIN=$1
}

DESKTOP=off
toggle_desktop() {
    test x$1 = x${DESKTOP} || \
	dbus_activate /org/freedesktop/compiz/core/allscreens/show_desktop_key
    DESKTOP=$1
}

ROTATE=off
toggle_rotate() {
    test x$1 = x${ROTATE} || {
	if test ${ROTATE} == off ; then
	    DIRECTION=left
	else
	    DIRECTION=right
	fi
	dbus_activate \
	    /org/freedesktop/compiz/rotate/allscreens/rotate_${DIRECTION}_key
	sleep 1
    }
    ROTATE=$1
}

cleanup() {
    echo 'Cleaning up...'
    toggle_desktop off
    toggle_snow off
    toggle_rain off
    exit 0
}

detect_lplayer() {
    ps -u ${USER} | grep -q lplayer
}

detect_block() {
    test `xquerypointer | gawk '{print $1}'` -eq 0
}


#-------------------------------------------------------------------------------
trap 'cleanup' INT TERM EXIT

xset dpms 0 600 900

while : ; do
    xscriptsaver --wait 60 && \
	! detect_lplayer && \
	! detect_block && {
	# Stage 1: one minute of inactivity
	toggle_rain on

	xscriptsaver 60 && {
	    # Stage 2: two minutes of inactivity
	    toggle_rain off
	    toggle_snow on

	    xscriptsaver 180 && {
		# Stage 3: five minutes of inactivity
		toggle_rotate on

		xscriptsaver 300 && {
		    # Stage 4: ten minutes of inactivity
		    toggle_snow off
		    toggle_rotate off	    
		    xscriptsaver 0
		}

		toggle_rotate off
	    }

	    toggle_snow off
	}

	toggle_rain off
	echo 'Desactivating screensaver'
    }
    
    test $? -eq 2 && {
	echo "$0: problem with X, bailing out..."
	exit 1
    }
done

#-------------------------------------------------------------------------------
