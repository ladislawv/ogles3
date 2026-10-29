ogles2 drawbench
================

What this is
------------
A benchmark of the library's CPU time per GL call: the calls a game makes
most (vertex attribute pointers, buffer updates, uniforms, binds, draws),
each timed alone, and one frame of Quake III's renderergl2 rebuilt from its
counts of calls. The same program runs on ogles2.library 1.3 and on
ogles3.library (in its default OpenGL ES 3.1 context and in an ES 2.0
one), so the lines compare the two libraries call by call. It draws a few
small triangles into a hidden 128x128 picture: nothing to see, and the GPU's
work does not count. About 10 seconds for each of the three runs.

How to run
----------
    CD <where you copied it>/ogles2-drawbench
    Execute Run-Bench

The results are in results/ (2.txt: ogles2, 3.txt: ogles3, 3es2.txt:
ogles3 in an ES 2.0 context). An ogles3.library installed in LIBS: is the
one that runs (results/libs.txt lists them).
