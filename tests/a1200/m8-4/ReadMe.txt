ogles2 M8 run 4
===============

What this is
------------
OpenGL ES 3 through SDL2 and SDL3: a program that asks SDL for an OpenGL
ES 3.0 context gets one from ogles2.library and draws with an OpenGL ES 3
shader; one that asks for ES 3.2 is told no; one that asks for ES 2.0
gets what it always got. SDL2's spinning cube runs once more through the
same code. Then the same questions go to a library as it would be
released today, which must say no to ES 3.0 with a clear message.

Small windows open and close on the Workbench; the whole run takes about
half a minute. Normally it runs remotely (amictl); it works by hand too.

Before running it, make sure no ogles2.library is installed in LIBS: (it
would be used instead of the package's): results/00-libs.txt lists what
LIBS: has.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m8-4
       Execute Run-M1
2. Fill in results/notes.txt.
3. Send me the whole results directory.
