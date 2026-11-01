Boing 2026 for OpenGL ES 3
==========================

Boing 2026 (Examples/boing2026.txt) remade with what ogles3.library's
OpenGL ES 3.0 contexts give that OpenGL ES 2.0 has not. It opens on a pool: the
ball dives into water whose waves, caustics on the bottom and light thrown
on the walls are all worked out on the GPU, lit by a coloured light that
flies round the ball or follows the mouse. Key 9 drains it to Boing 2026's
lacquered floor, where sparks fly at each hit and the shadows harden where
the ball touches. Boing 2026's own passes and shaders are inside it too:
key E switches between the two ways, so it compares them itself.


Running it
----------
Needs ogles3.library 1.0 (in LIBS:, or in libs/ beside the program) and a
32-bit RTG screen. The shaders come compiled (shaders/), so the shader
compiler is not needed.

    boing2026-ogles3 [WIDTH=w] [HEIGHT=h] [FRAMES=n] [NOMSAA] [NOSOUND]
                     [FULLSCREEN] [TIMING] [SHOWMAP] [VSYNC] [TOUR]
                     [SCALE=percent] [ES2] [DRY] [SOUND]

In a 640 x 480 window by default; FULLSCREEN opens a 1280 x 720 screen of
its own (WIDTH and HEIGHT for another size).

  TOUR       switches the effects off and on one by one, every 5 seconds, and
             says in the window's title what each one proves. Start here.
  ES2        starts the ES 2.0 way (key E)
  DRY        starts with the lacquered floor instead of the pool (key 9)
  SOUND      plays the boing on Paula (silent by default)
  The others as Boing 2026's: TIMING the time of each pass every 120
  frames, SHOWMAP the shadow maps (key M), VSYNC waits for the display,
  SCALE the size the room is drawn at (by default 100% up to 640 x 480;
  50% at 1280 x 720), NOMSAA. NOSOUND is still taken (silent anyway).

Keys: 1 shadow  2 reflection  3 bloom  4 per-pixel / per-vertex light
      5 checker antialiasing  6 the 1984 look  7 sparks
      8 contact-hardening shadows  9 the pool or the dry floor
      E the ES 2.0 way
      [ ] the water's depth in the middle  + - the waves' tilt in the light
      the water reflects on the walls (fine steps)  M shadow maps
      T time the passes  L stop or start the lights  Space pause  Esc quit.
      Drag with the left mouse button to move the camera. Moving the mouse
      moves the coloured light; 5 seconds after it stops, the light flies on.


How fast
--------
Measured on a Raspberry Pi 4 in an A1200's PiStorm, ogles3.library 1.0,
full screen, 4x MSAA, frames not limited (no VSYNC), every effect on:

                                  Boing 2026   ES 2.0 way   ES 3 way   ES 3 way
                                                            dry floor  the pool
    640 x 480    room at 100%     60.2         60.1         54.3       33.9
    1280 x 720   room at 50%      59.6         59.1         52.5       34.8
    1280 x 720   room at 100%     24.8         24.7         22.6       17.2
                                  frames a second

The ES 3 way on the dry floor draws the sparks and the contact-hardening
shadows on top; the others draw none. The pool's passes (its waves, its
caustics and the light it throws on the walls, about 9 ms at 640 x 480) are
what it costs: its room shaders carry the water's code even when the floor
is dry, which is why the dry floor runs below the ES 2.0 way. With VSYNC it
is held to the display's rate. The passes on the dry floor at 640 x 480 as
first measured, before the pool (TIMING, the GPU finishing each), against
the ES 2.0 way's:

    shadow maps    0.9 ms   (1.0)    both in one pass
    reflection     0.9 ms   (0.6)    the ball and the sparks, half floats
    room           9.7 ms   (10.4)   two targets at once
    bloom          2.2 ms   (2.5)    no second drawing of the room
    window         3.7 ms   (2.8)    with the sparks and the flare


What each change proves
-----------------------
1. The bloom's bright part from the room's own pass (several render targets)
   The room's shader writes its colour and its part brighter than white
   into two targets at once (glDrawBuffers, layout(location = 1) out). The
   ES 2.0 way draws the whole room a second time, at a quarter of the size,
   for that. A quarter-size pass then brings the bright part down (four
   bilinear reads, the texel size from textureSize), the ball and the
   sparks add theirs there, and the blurs follow as before.

2. HDR bloom (float render targets)
   The bright part and the bloom are R11F_G11F_B10F (EXT_color_buffer_float:
   32 bits a pixel, half RGBA16F's bandwidth), the reflection RGBA16F. The
   ES 2.0 way stores a quarter of the value in 8 bits and multiplies it back:
   steps in the glow, what is above white clipped at 4. Here the glow keeps
   its values (held at 6 above white, so the clear-coat's peaks do not
   blot).

3. Shadows filtered by the texture unit (shadow samplers)
   Both lights' shadow maps are drawn in one pass into the two halves of one
   depth texture, read through a sampler2DShadow with GL_LINEAR: each read
   compares 2x2 depths and filters them. 6 taps a light give a smoother
   penumbra than the ES 2.0 way's 12 and 8 nearest ones. M shows the maps
   through a sampler object that turns the comparison off.
   Watch for: a count of taps from a uniform keeps a real loop, each read
   waited for (0.7 ms more for the room at 640 x 480); a constant count is
   unrolled and the reads overlap.

4. Fewer calls on the 68k (uniform blocks, vertex array objects, instancing)
   The frame's data is one uniform block (std140) written once a frame with
   one glBufferSubData and read by every program, where the ES 2.0 way sets
   about 40 uniforms. Vertex array objects hold the sphere's and the room's
   arrays. The screen triangle and the flare have no vertex data:
   gl_VertexID makes the corners, gl_InstanceID picks the flare's piece
   from a constant array (one draw, not 8).

5. The room in two programs
   The floor has a program of its own: its normal is a constant, so the
   compiler folds the lighting's dot products and the mirrored ray, and
   the walls and ceiling no longer carry the floor's reflections (1,167
   instructions where the one room shader had 1,451). The walls' normal is
   flat (one a triangle).

6. Sparks at every impact (instancing, integers in shaders)
   640 streaks for each impact still throwing sparks (128 of them in the
   reflection and the bloom), one instanced draw:
   each spark's direction, speed and life are integer hashes of
   gl_InstanceID, its path (thrown, falling, bouncing once on the floor) is
   worked out in the vertex shader from a block of the last impacts. No
   vertex data and no CPU work per spark. Drawn into the window, the
   floor's reflection and the bloom; a spark behind the ball is hidden by
   a test against the sphere (there is no depth buffer).
   Pi 4: about 0.4 ms in the window while sparks fly.

7. Contact-hardening shadows (key 8)
   With the filtering in the texture unit the penumbra can follow the
   distance from the ball: sharp where the ball touches the floor, soft
   as it rises.

8. Water on the floor (key 9: float render targets, meshes made from
   gl_VertexID, additive float blending)
   The floor becomes a pool, no depth at the walls and deepest in the
   middle ([ and ]). With it the ceiling lamp is off: the coloured light
   flying round the ball lights the room.
   - The waves: heights simulated on the GPU in two 128 x 128 RGBA16F
     textures, each frame one drawn from the other (the wave equation,
     slower in the shallows), the ball diving in and coming out pushing
     and pulling the water; small running waves on top. The CPU reads
     nothing back.
   - The surface: its normal from the waves' slope; the bottom seen
     through it where the bent view ray meets the curved bottom (the grid
     refracted, tinted with the depth); a sharp glint of the light; the
     ball below the surface seen through the water.
   - Caustics on the bottom, traced: the surface as a 96 x 96 mesh made from
     gl_VertexID, each vertex refracting the light's ray onto the bottom;
     each triangle adds its light over its new area into a float texture
     (focused: bright lines; spread: dim).
   - The light the water reflects on the walls and the ceiling, traced the
     same way: a 64 x 64 mesh, each ray reflected off the waves (+ and -
     set how much of their tilt) and followed to the back wall, a side wall
     or the ceiling, a quarter of a texture each. Each vertex works out its
     whole triangle, so one that misses or is stretched away is dropped
     before a pixel is drawn (fading out, not popping).
   - A dive throws a crown of drops instead of sparks.
   Pi 4: about 35 frames a second at 640 x 480 with it on (the water's
   passes about 9 ms, the room 14).


How it is put together
----------------------
The ES 3 way, five passes a frame: the shadow maps; the mirrored ball and
sparks; the room at the render scale into two targets; the bloom (the
bright part brought down, the ball and sparks added, blurred twice each
way); the window: the room brought up with the bloom and the vignette, the
ball with 4x MSAA, then the sparks and the flare added. The ES
2.0 way is Boing 2026's, unchanged (shaders/*.vert and *.frag without a 3).

The shadow maps would be tidier as the two layers of a depth array texture,
but ogles3 1.0 attaches no layer of one to a framebuffer.


Building
--------
In WSL with bebbo's amiga-gcc and, for the compiled shaders, the offline
compiler of the ogles3 sources:

    make          boing2026-ogles3
    make header   boing2026-ogles3_shaders.h from shaders/
    make shaders  build/v3dp/<hash>.v3dp, .v3dw, .v3ds

A changed shader needs new binaries (make shaders).


MIT License. The Boing Ball is the Amiga's, by Dale Luck and R.J. Mical,
1984.
