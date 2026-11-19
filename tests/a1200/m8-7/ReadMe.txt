ogles2 M8 run 7
===============

What this is
------------
A test of the render chain in ogles3.library: a framebuffer object's job
flushed while the previous render still runs is binned at once and its
render queued behind the running one on the GPU (up to two renders at
once). m8_queue opens a 640x480 screen of its own; every frame it clears a
small texture to a colour of that frame's own and draws it over the screen,
and every second frame reads the screen back: it must be that frame's
colour. Then two cases the chain must not take: a vertex shader that
reads the texture just rendered (H1), and transform feedback into the
buffer the object drew from (H2). A second context's life must give back
all its memory. Step 2 runs the same with the chain turned off
(ENV:OGLES3/NoQueue), the control. About ten seconds in all.

Before running it, make sure no ogles3.library is installed in LIBS: (it
would be used instead of the package's): results/00-libs.txt lists what
LIBS: has.

How to run by hand
------------------
1. Open a Shell and type:
       CD <where you copied it>/ogles2-m8-7
       Execute Run-M1
2. Watch the screen change colour; it closes by itself.
3. Write a few words into results/notes.txt.

What to send back
-----------------
The results/ directory. results/1-chain.txt and 2-noqueue.txt end with
"PASSED: n of n checks passed"; results/1-chain.log says how many jobs
were queued ("jobs queued behind a running render").
