ogles2-swarm: Boing Swarm's step 1, the compute benchmark
==========================================================

How fast the Pi 4's GPU runs a swarm of boids through ogles3's compute
shaders (docs/GLES3_PLAN.md, "Plan: Boing Swarm"). Each run draws the boids
as small lights on a spinning Boing ball, 640x480 full screen (MSAA:
antialiased).

  CD to this directory, then:   Execute Run-Swarm

bin/swarmbench by hand:

  swarmbench [N=boids] [MODE=free|grid|vbo|draw] [MAX=n] [FRAMES=n]
             [WIDTH=w] [HEIGHT=h] [FULLSCREEN] [MSAA]

  draw   the draw alone, no simulation
  free   one compute pass: each boid pulled to its place, no neighbours
  grid   the neighbour grid (scan, scatter) and flocking, half the
         boids a frame, at most
         MAX neighbours a boid (8), each every EVERY frames (3)
  vbo    free, drawn from the storage buffer as vertices (copied back
         through the 68k by the library)

Each prints frames a second and the time of each part a frame. Results go
to results/. Esc quits a run.
