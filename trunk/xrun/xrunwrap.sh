#! /bin/sh
# Bourne shell wrapper to simplify life with xrun: check for missing program,
# and execute the line the the background as appropriate... Put in your path,
# and invoke as:
# 
# sh # xrun -p 'xrunwrap.sh '
#
# This way, you can have a fast returning xrun without having to worry about
# most obvious typos...
#
die() {
    echo ${*} 
    exit 1
}

if test ${#} -gt 0 ; then
    which ${1} > /dev/null 2>&1 || die "program ${1} not found"
    $* &
else
    die "no program given"
fi
