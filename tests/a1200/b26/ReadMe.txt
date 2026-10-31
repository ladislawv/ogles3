ogles2 b26: Boing 2026 measured
==============================

What this is
------------
The frame rate and the time of each pass of Boing 2026 (docs/GLES3_PLAN.md,
"Plan: Boing 2026 for ogles3"): the GLES2 example on ogles2.library 1.3.2
(bin2/) and on ogles3.library (bin3/), and its OpenGL ES 3 copy,
boing2026-ogles3 (bin3/), in its own way and the ES 2.0 way (ES2, key E).
Each at 640x480 with the room at 100%, and at 1280x720 with the room at 50%
and 100%: full screen, 4x MSAA, frames not limited, the passes timed
(glFinish between them) every 120 frames. About 10 seconds a run.

How to run
----------
    CD <where you copied it>/ogles2-b26
    Execute Run-B26

The results are in results/: 2-*.txt ogles2, 3-*.txt ogles3 (the GLES2
program), 3n-*.txt the ES 3 copy, 3e-*.txt the copy the ES 2.0 way. An
ogles3.library installed in LIBS: is the one that runs (results/libs.txt
lists them).
