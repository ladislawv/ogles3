Boing Ocean for OpenGL ES 3
===========================

Boing Ocean (Examples/boingocean, the SDK's example 3) remade with what
ogles3.library's OpenGL ES 3.0 contexts give that OpenGL ES 2.0 has not: the
Boing ball floating on a ray-marched ocean, now in high dynamic range, with
a cloudy sky, sun glints on the ripples that bloom, a ball whose checkers no
longer crawl, and caustics playing on it under the water. Boing Ocean's own
shaders are inside it too: key E switches between the two ways, so it
compares them itself.


Running it
----------
Needs ogles3.library 1.0 (in LIBS:, or in libs/ beside the program) and a
32-bit RTG screen. The shaders come compiled (shaders/), so the shader
compiler is not needed.

    boingocean-ogles3 [WIDTH=w] [HEIGHT=h] [SCALE=1..4] [FRAMES=n]
                      [FULLSCREEN] [ES2] [TIMING]

It starts the OpenGL ES 3 way with every effect on, in a 640 x 480 window;
FULLSCREEN opens a 1280 x 720 screen of its own (WIDTH and HEIGHT for
another size). ES2 starts the ES 2.0 way (key E), SCALE draws the scene at
1/2..1/4 of the size (keys 1-4), FRAMES quits after n frames and prints the
average frame rate, TIMING the time of the passes.

Keys: E the ES 2.0 way or OpenGL ES 3  R ripples from a texture or from
      sines  G glints  C clouds  U caustics on the ball  B bloom
      1-4 the render scale (1/1..1/4)  0 the ES 3 way at 75%  Esc quit.
      Drag with the left mouse button to turn and raise the camera; it
      also circles the ball by itself. The window's title shows the frame
      rate and what is on.


How fast
--------
Measured on a Raspberry Pi 4 in an A1200's PiStorm, ogles3.library 1.0,
full screen, frames not limited, every effect on, scale 1/1:

                     Boing Ocean    ES 2.0 way    OpenGL ES 3 way
    640 x 480        59.1           61.6          49.5
    1280 x 720       20.9           21.3          19.7
                     frames a second

The ES 3 way draws more for its frame: the clouds, the blooming glints, the
ball's caustics and the high dynamic range cost about 4 ms a frame at
640 x 480. Key 0 draws it at 75% and brings it up to the window in the pass
that tone maps it, for a faster frame.


What it shows of OpenGL ES 3
----------------------------
- Half-float render targets: the coarse water (height and slope) in
  RGBA16F, no 8-bit steps in its slope (the ES 2.0 way packs it in bytes,
  and its normal bands); the scene itself in R11F_G11F_B10F, so what is
  brighter than white (the sun's glints, the ball's highlight) keeps its
  value until the tone mapping.
- textureLod in the ray march: no implicit level of detail inside a loop
  whose rays part ways.
- The ripples a small tiling texture drawn once a frame: two reads a pixel
  in place of three sines.
- The sky a half-float cube map, its six faces drawn when the sun has
  moved, with clouds drifting through a 3D noise texture (sampler3D): one
  read for every reflection, the sun's glow in it.
- The ball's checkerboard a mipmapped cube map read with the ball's own
  direction: no atan/asin per pixel, and its edges filtered by the mips
  instead of crawling as it turns.
- Bloom: the bright part at half size, blurred, added back with ACES and a
  vignette in one pass.
- The frame's data one uniform block written once a frame; the full-screen
  passes made from gl_VertexID with no vertex data.
- Caustics on the ball where it is under the water, from the ripple
  texture.


Building
--------
In WSL with bebbo's amiga-gcc and, for the compiled shaders, the offline
compiler of the ogles3 sources:

    make          boingocean-ogles3
    make header   boingocean-ogles3_shaders.h from shaders/
    make shaders  build/v3dp/<hash>.v3dp, .v3ds

It includes Boing Ocean's shaders from ../boingocean_shaders.h (its ES 2.0
way). A changed shader needs new binaries (make shaders).


MIT License. The water's wave model and sky are afl_ext's "Very fast
procedural ocean" (https://www.shadertoy.com/view/MdXyzX, MIT License,
Copyright (c) 2017-2024 afl_ext); see ../boingocean_shaders.h. The Boing
Ball is the Amiga's, by Dale Luck and R.J. Mical, 1984.
