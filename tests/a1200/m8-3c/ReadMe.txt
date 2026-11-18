ogles2 M8 run 3c
================

What this is
------------
dEQP-GLES31, the OpenGL ES 3.1 conformance tests of Khronos' test suite
(VK-GL-CTS), compiled for the Amiga (68020 + 68881) and run on a test
build of ogles3.library that offers OpenGL ES 3.1 contexts (not yet
complete: most groups are expected to fail for now). It draws in a small
256x256 window on the Workbench screen, one test after another, and
checks each result itself: nothing needs watching.

There are about 30,000 tests; a run takes a group of them (a pattern such
as dEQP-GLES31.functional.fbo.*). The first time a test runs, its shaders
are compiled on the Amiga, which takes about a second each; they are kept
in cache/.

This is M8 run 3 with one difference: the built-in functions' precision
tests (shaders.builtin_functions.precision, and dEQP-GLES31's
builtin_functions.precision) check their values also on the Raspberry
Pi's free ARM cores, through core3.library (the Core 3 SDK, in LIBS:):
bin/deqp-c3.c3m is that code. The m68k then looks only at the values
flagged there, so those tests take a fraction of their time. The results
are the same. Without core3.library, or with
    SetEnv DEQP_C3 0
everything is checked on the 68k as in run 3. A "Core 3:" line in
results/<NAME>.txt says which, another at its end how many cases went there.
    Execute Run-M1 CASES=... VERIFY=1
checks every value on both sides and notes where they differ.

Normally it runs remotely (amictl); it works by hand too. Please do not
run another 3D program (Quake, MiniGL) at the same time.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m8-3c
       Execute Run-M1
   That runs the information tests (a few seconds). A group of the
   official list (the files in caselists/):
       Execute Run-M1 LIST=fbo
   Tests by name pattern:
       Execute Run-M1 CASES=dEQP-GLES31.functional.draw_indirect.* NAME=clear
   Ctrl-C stops it after the current test.
2. Fill in results/notes.txt.
3. Send me the whole results directory.

What is in it
-------------
bin/deqp-gles31       the tests (ports/deqp in the ogles2 sources)
bin/gles31/           their data: shader test files, a few images
bin/deqp-c3.c3m      the precision checks for the ARM cores
bin/OGLES3Log        shows the library's log
libs/                ogles3.library (an OpenGL ES 3 test build) and the
                     shader compiler, used from here for the run only
cache/               compiled shaders
caselists/           the official list of tests, a file per group
Run-M1               the script

If the machine hangs or crashes
-------------------------------
Reset it, then send results/ as it is: 00-progress.txt, <NAME>.txt and
<NAME>.qpa show how far it got.
