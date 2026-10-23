ogles2 M8 run 2
===============

What this is
------------
dEQP-GLES3, the OpenGL ES 3.0 conformance tests of Khronos' test suite
(VK-GL-CTS), compiled for the Amiga (68020 + 68881) and run on a test
build of ogles2.library that offers OpenGL ES 3.0 contexts (not yet
complete: most groups are expected to fail for now). It draws in a small
256x256 window on the Workbench screen, one test after another, and
checks each result itself: nothing needs watching.

There are about 45,000 tests; a run takes a group of them (a pattern such
as dEQP-GLES3.functional.fbo.*). The first time a test runs, its shaders
are compiled on the Amiga, which takes about a second each; they are kept
in cache/.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program (Quake, MiniGL) at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m8-2
       Execute Run-M1
   That runs the information tests (a few seconds). A group of the
   official list (the files in caselists/):
       Execute Run-M1 LIST=fbo
   Tests by name pattern:
       Execute Run-M1 CASES=dEQP-GLES3.functional.color_clear.* NAME=clear
   Ctrl-C stops it after the current test.
2. Fill in results/notes.txt.
3. Send me the whole results directory.

What is in it
-------------
bin/deqp-gles3       the tests (ports/deqp in the ogles2 sources)
bin/gles3/           their data: shader test files, a few images
bin/OGLES2Log        shows the library's log
libs/                ogles2.library (an OpenGL ES 3 test build) and the
                     shader compiler, used from here for the run only
cache/               compiled shaders
caselists/           the official list of tests, a file per group
Run-M1               the script

If the machine hangs or crashes
-------------------------------
Reset it, then send results/ as it is: 00-progress.txt, <NAME>.txt and
<NAME>.qpa show how far it got.
