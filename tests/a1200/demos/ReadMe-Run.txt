ogles2 demos run
================

The release's demos archive (release/dist/ogles3-<release>-demos.zip,
made by hand) unpacked as a user unpacks it: Boing 2026, Boing Ocean,
Boing Swarm and Juggler RT with libs/ogles3.library and their compiled
shaders in shaders/. Each runs 150 frames in a window, the OpenGL ES 3
way, then the ES 2.0 way (Juggler also MODERN). Each run's library log
is results/<n>-<demo>.log: no "compiler:" line in any of them means every
program's binary came from shaders/, as the archive promises.

CD to this directory, then:   Execute Run-M1
