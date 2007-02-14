#! /bin/bash

help2man -N $* > tmp.txt
man -- ./tmp.txt | python `dirname $0`/groff2html.py
rm tmp.txt
cat > /dev/null 2>&1
