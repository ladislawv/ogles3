Boing Swarm for OpenGL ES 3.1
=============================

16384 tiny lights fly together as a flock, and the flock is a spinning
red-and-white Boing ball, gliding in a spiral over a checkered floor. It
bursts apart, flies as a murmuration, and swarms back together, stripe by
stripe. Every boid is simulated by the GPU's compute shaders, every frame;
the 68k only tells the show where it is.

  boingswarm-ogles3 [N=boids] [WIDTH=w] [HEIGHT=h] [FULLSCREEN] [MSAA]
                    [FRAMES=n] [EVERY=n] [TIMING] [SOUND] [ES2] [CAPTURE]
                    [VSYNC] [NOFLOOR]

It opens a 640 x 480 window or, with FULLSCREEN, a 1280 x 720 screen (WIDTH
and HEIGHT for another size). N: the boids (16384; a multiple of 128).
EVERY: how many frames between a boid's looks at its neighbours (4: each
frame a quarter of the boids look). MSAA: 4x antialiasing (off: added-up
lights need none). TIMING prints each pass's time, for each part of the
show and at the end. SOUND plays the boing at the low point of each of the
ball's sinks and a whoosh at the burst (silent by default). ES2 starts the
ES 2.0 way, CAPTURE in capture mode (keys E and C). VSYNC waits for the
display (no tearing). NOFLOOR starts without the floor and the lights'
reflection in it (key F).

The mouse is a hawk: the swarm flees from it, and it dents the ball's
skin. Keys:
  E      the ES 2.0 way and back: the same show with the same boids simulated
         by the 68k (the same neighbour grid), drawn as OpenGL ES 2.0
         draws: GLSL ES 1.00, vertex arrays the CPU fills each frame, no
         compute, no instancing, no float targets, no bloom; the text
         spells OGLES2
  F      the floor and the reflection on and off
  S      the stats: who simulates how many boids, frames a second, and the
         68k's own work a frame (the show and the camera; in the ES 2.0
         way the swarm too)
  C      capture mode: no text, no pointer, and every frame 1/60 s of the
         show (smooth in a recording whatever the frame rate)
  Space  pause         Esc  quit

The show (it loops from the Boing on):
   0 s  the gathering: sparks pour in from far out and fold into the ball;
        an eighth of the boids spell OGLES3, scrolling in from the right as
        they form, that turn into Saturn's rings round the ball and soon
        break into a storm swirling round it
  12 s  the Boing: the ball spins and glides round in a spiral, rising and
        sinking; every 3 seconds a drop falls at a random place and ripples
        its skin
  35 s  the burst: it explodes into a storm of light, flashing white, that
        slides across the floor
  37 s  the murmuration: the swarm flies in great flowing shapes
  55 s  the return: the ball rebuilds stripe by stripe from the bottom, red
        boids to red places, white to white

What OpenGL ES 3.1 does here:
  - compute shaders: the whole simulation (flocking, the pull to each
    boid's place on the ball or in the letters, the burst, the flowing
    murmuration)
  - storage buffers: every boid's position and speed, on the GPU from
    start to end
  - atomics and shared memory: a neighbour grid (each boid counted into a
    cell, the cells summed, the boids sorted), so each boid looks only at
    nearby ones
  - the ball's skin is water: a wave simulation over its surface (128 x
    64 cells, a step a frame) that the boids ride on
  - half-float render targets for the trails and the bloom, at quarter
    size, added over the screen
  - an image written by the compute shader and read by the vertex shader
    with texelFetch (gl_VertexID); each light a point sprite
  - the lights' reflection in the floor: the same draw again, mirrored in
    the vertex shader

The shaders' compiled forms are in the shaders/ drawer beside the program.

On a PiStorm's Raspberry Pi 4, full screen at 1280x720: about 60 frames a
second while the swarm flies with trails, 75 on the formed ball (the 68k's
own work 0.1 ms a frame).

MIT License.
