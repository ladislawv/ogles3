ogles2 M8 run 1
===============

What this is
------------
The first test of the OpenGL ES 3 work (stage A): contexts of OpenGL ES
3.0 next to OpenGL ES 2.0 ones, their version strings, extensions and
limits, and the rule that a GLES2 context refuses GLSL ES 3.00 shaders. It
runs twice: with a library as it would be released (ES 3.0 refused) and
with a test build that gives ES 3.0 contexts.

Nothing draws: there is nothing to watch. The whole run takes less than a
minute. Normally it runs remotely (amictl); it works by hand too.

Before running it, make sure no ogles2.library is installed in LIBS: (it
would be used instead of the package's): results/00-libs.txt lists what
LIBS: has.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m8-1
       Execute Run-M1
2. Fill in results/notes.txt.
3. Send me the whole results directory.

If the machine hangs or crashes
-------------------------------
Reboot and send the results directory as it is: ogles2.log is written line
by line, so its last lines show how far it got. To go on after the step
that hung:   Execute Run-M1 FROM=<the next step>

Contents
--------
Run-M1                       the test script
libs-release/ogles2.library  the library as a release builds it
libs-es3/ogles2.library      a test build with OpenGL ES 3.0 contexts
bin/m8_es3                   the OpenGL ES 3 test
bin/smoke                    the library's smoke test
bin/OGLES2Log                writes the library's log file on request
notes-template.txt           copied to results/notes.txt at the end
