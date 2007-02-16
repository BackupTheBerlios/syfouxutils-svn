#! /bin/sh
#
# Invoke the autotools...
#
# ./autogen.sh			Should create the ./configure script
# ./autogen.sh --clean  	Should clean up the whole autotools scripts
#
# Tested on autoconf 2.61 and automake 1.10.
#
set -x
if test "x$1" = "x--clean" ; then
    find . -mindepth 1 -maxdepth 1 \
	-not -name 'autogen.sh'    \
	-not -name 'configure.in'  \
	-not -name 'Makefile.am'   \
	-not -name 'xrun.c'        \
        -not -name 'xrunwrap.sh'   \
	-not -name '.svn'          \
	-exec rm -rf \{\} ';'
else
    for F in README NEWS AUTHORS ChangeLog ; do touch ${F}; done
    aclocal
    autoheader
    automake --add-missing
    autoconf
fi
