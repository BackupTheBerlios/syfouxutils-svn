"""
Usage: python ./groff2html.py <input >output

Basically, the terminal groff output (through grotty) double print using ASCII
backspace (character 8) to notify highlighting. This way, when you cat a
document to a valid terminal, you just see the normal output... -- Sylvain
"""

from sys import stdin, stdout
from re import compile 
from cgi import escape

def bufferize(inbuf):
    """First pass: find individual character state"""
    buf = []
    it = iter(inbuf)
    for c in it:
        if ord(c) == 8:
            buf[-1] = (it.next(), True)
        else:
            buf.append((c, False))
    return buf

def chunks(buf):
    """Second padd: group them"""
    state = False
    chunk = ''
    for c, s in buf:
        if s == state:
            chunk += c
        else:
            yield chunk, state
            state = s
            chunk = c
    yield chunk, state

eol = compile('(\n)')

print '<tt><pre>'
for chunk, state in chunks(bufferize(stdin.read())):
    for item in eol.split(escape(chunk)):
        if item != '\n':
            if len(item) > 0:
                stdout.write('<span class="%s">%s</span>' %
                             (('low', 'high')[state], item))
        else:
            stdout.write('<br />\n')
print '</pre></tt>'
