ogles2 M8 run 5
===============

What this is
------------
The first test of the OpenGL ES 3.1 work (stage D) on the Amiga: a test
build of ogles2.library that offers OpenGL ES 3.1 contexts, its new state
and limits, and the OpenGL ES 3 drawing tests run in such a context,
including the new ways of feeding vertices to the GPU. Everything draws
into a hidden bitmap: there is nothing to watch. The whole run takes less
than a minute. Normally it runs remotely (amictl); it works by hand too.

Before running it, make sure no ogles2.library is installed in LIBS: (it
would be used instead of the package's): results/00-libs.txt lists what
LIBS: has.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m8-5
       Execute Run-M1
2. Fill in results/notes.txt.
3. Send me the whole results directory.
