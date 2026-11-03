ogles2-juggler: Juggler RT, Eric Graham's Juggler ray traced live
=================================================================

How fast the Pi 4's GPU ray traces the 1987 Juggler through ogles3's compute
shaders (docs/GLES3_PLAN.md, "Plan: Juggler RT"): the robot of robot.dat
juggling three mirror balls, 640x480 full screen.

  CD to this directory, then:   Execute Run-Juggler

Each run prints frames a second and the time of each part a frame
(animate, trace, ham, show). Results go to results/. Esc quits a run.

bin/juggler-ogles3 by hand (keys: M 1987/2026, E the ES 2.0 way, N the next
scene, O the camera round, R the camera back, + and - 2026's size, Space
pause, Esc quit; drag the mouse to turn the camera):

  juggler-ogles3 [SCENE=file.dat] [WIDTH=w] [HEIGHT=h] [FULLSCREEN]
                 [MODERN] [SCALE=percent] [ES2] [FRAMES=n] [TIMING] [VSYNC]
