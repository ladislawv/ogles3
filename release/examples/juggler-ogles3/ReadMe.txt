Juggler RT for OpenGL ES 3.1
============================

Eric Graham's Juggler (1987), ray traced live. In 1987 his ray tracer took
about an hour for one picture of the robot juggling three mirror balls on
an Amiga; here every pixel of every frame is traced by the GPU's compute
shaders, from his own sums (rt1.c, rt2.c, rt3.c) and his scene files.

  juggler-ogles3 [SCENE=file.dat] [WIDTH=w] [HEIGHT=h] [FULLSCREEN]
                 [MODERN] [SCALE=percent] [ES2] [FRAMES=n] [TIMING] [VSYNC]

It opens a 640 x 480 window or, with FULLSCREEN, a 1280 x 720 screen (WIDTH
and HEIGHT for another size). It starts in 1987 and with the juggler.

  1987   320 x 200 and 4-bit colour as the original drew it, shown through
         the HAM6 screen it was made for: each pixel either one of 16
         colour registers or the one on its left with one channel
         changed (the fringes). The registers are allocated as rt3.c did,
         over the scene's first picture.
  2026   the light itself at the window's size (SCALE: a share of it,
         100%). Once nothing moves (Space, or a still scene) the pictures
         are added up, each with its rays moved within the pixel and the
         shadows' rays to another point of the lamp: smooth edges and
         soft shadows in a second or two.

SCENE: a scene file of the 1987 ray tracer (robot.dat, ele.dat and
dragon.dat are built in, key N). ES2 starts the ES 2.0 way, TIMING prints
each pass's time at the end, FRAMES quits after that many frames.

Keys:
  M      1987 and 2026
  E      the ES 2.0 way and back: the same ray tracer as a fragment shader
         (GLSL ES 1.00), every ray against every sphere, the spheres in
         uniform arrays the 68k fills each frame (it also does the juggling)
  N      the next scene: the juggler, robot.dat still, ele.dat, dragon.dat
  O      the camera: swinging slowly left and right (as it starts, in
         every scene), round and round, still
  R      the camera back where the scene file put it, swinging
  H      the text: the scene, the way, frames a second and the keys
  + -    2026's size
  Space  pause         Esc  quit
The mouse: drag to turn the camera round the scene.

How the ES 3.1 way works, a frame:
  animate.comp  the juggling: robot.dat's 22 points (the balls, hands,
                elbows by two-bone IK, knees, the body bobbing) at this
                moment, then the 79 spheres between them and each object's
                bounding sphere, in storage buffers. The 68k sends the time.
  shadow.comp   the lamp's view: a shadow ray is a line through the lamp,
                so seen from it a point. A 16 x 16 grid lists in each cell
                the spheres whose disc, seen from the lamp, reaches it.
  trace.comp    rt1.c, a 16 x 16 tile a work group: first the spheres in
                the tile's view are gathered into shared memory (an atomic
                counter), so a ray from the eye tests only those; a shadow
                ray only its cell's spheres; reflected rays go through the
                objects' bounding spheres. Into an image (imageStore).
  ham.comp      1987 only: one invocation a row, the HAM decoding, the
                nearest colour register of each colour looked up.

Files: juggler-ogles3.c (the Amiga program), juggler_gl.h (the renderer)
and juggler_scene.h (the scenes, the juggling, HAM) are shared with
tools/pccheck.c, which runs the same renderer and shaders on a PC (Mesa)
and compares robot.dat's picture with the original program's.

Eric Graham's ray tracer, by his word: "anyone can do what they want with
the code, so long as I get a mention". Thanks to Chris Hanson (AlphaPixel),
who recovered and published it. This program: MIT License.
