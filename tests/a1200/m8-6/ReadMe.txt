ogles2 M8 run 6
===============

What this is
------------
A hunt for a crash: some of the OpenGL ES 3.1 conformance tests that draw
through "indirect" commands make the Amiga restart now and then. This
package repeats their drawing in a 256x256 window, many times over, so
that the restart comes when asked for and can be narrowed down. It may
restart the machine: save your work first. Normally it runs remotely
(amictl); it works by hand too.

Before running it, make sure no ogles2.library is installed in LIBS: (it
would be used instead of the package's): results/00-libs.txt lists what
LIBS: has.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m8-6
       Execute Run-M1
2. A window shows triangles changing quickly for a minute or two.
3. Fill in results/notes.txt.

ARGS="1000 direct" and the like change what it draws (see Run-M1).
