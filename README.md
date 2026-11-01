# ogles3.library: OpenGL ES 3.1 for the classic Amiga

**Modern GPU graphics for AmigaOS 3.x.** On an Amiga with a
[PiStorm](https://github.com/captain-amygdala/pistorm) and a Raspberry Pi 4, `ogles3.library`
gives 68k programs real OpenGL ES 3.1, with compute shaders and GLSL ES 3.10, drawn by the Pi's
VideoCore GPU (Broadcom V3D 4.2) under [Emu68](https://github.com/michalsc/Emu68).

It is the same API that phones, browsers and game engines use, and the same `agl*` context calls
AmigaOS 4's ogles2.library has: OpenGL ES 2.0, 3.0 and 3.1 code from Linux, Android, the web or
OS4 compiles and runs on the classic Amiga.

**Version 1.0, release 1.0-beta1.** A beta: the complete conformance runs of OpenGL ES 2.0, 3.0
and 3.1 are made (dEQP-GLES31: none failed; see below); two rare freezes seen in them are still
being looked at. Please report what goes wrong.

ogles3.library is the successor of [ogles2.library](https://github.com/ladislawv/ogles2) 1.x
(OpenGL ES 2.0), built from the same source, as a library of its own: both can be installed at
once, and programs written for ogles2 keep using it.

## Why it matters

- **OpenGL ES 3.1.** Compute shaders, shader storage buffers, image load/store, atomic
  counters, indirect draws, separate shader programs, multisample textures; and all of OpenGL
  ES 3.0: instancing, uniform buffers, vertex array objects, transform feedback, multisampled
  framebuffer objects, integer and float render targets, 3D and array textures, sampler
  objects, ETC2/EAC, sync and queries. Programs get an OpenGL ES 3.1 context by default, an
  OpenGL ES 2.0 one when they ask.
- **Real shaders.** GLSL ES 1.00, 3.00 and 3.10, compiled by Mesa's own V3D compiler, running
  *on the Amiga*: a program hands the library its shader source, and it is compiled at link
  time and cached on disk. Or ship the compiled binaries with the program and start instantly.
- **Fast.** Measured with ogles2.library 1.3 (the same drawing core): Quake III with its GLES2
  renderer runs about **30% faster** than on MiniGL (timedemo `four` at 640x480: 80.4 against
  61.3 fps); the gears scene at 1734 fps against MiniGL's 791, and with 32,000 triangles 445 fps
  against 47.
- **Proven against the standard.** Khronos' dEQP-GLES31 tests on a real A1200, group by group:
  every case that applies passes (only those of OpenGL ES 3.2 and of extensions not offered report
  "not supported"). dEQP-GLES3 passed group by group as OpenGL ES 3.0 was built, and all 17,165
  dEQP-GLES2 cases passed in ogles2 1.3, but for 25 cube-map cases where the GPU filters edges
  seamlessly (as GLES 3 requires).
- **Complete.** All of OpenGL ES 3.1 (366 GL entry points) and 51 extensions: KHR_debug,
  float and half-float render targets, S3TC/DXT and ETC1 compression, clip control, anisotropic
  filtering, 4x MSAA with alpha-to-coverage, debug markers and more. OpenGL ES 2.0 contexts get
  ES 3.0's features as GLES2 extensions (OES_texture_3D, EXT_texture_storage, EXT_texture_rg,
  OES_depth24, EXT_occlusion_query_boolean, EXT_shadow_samplers, ...). Shared contexts across
  tasks, and vsync without tearing.
- **Ready for SDL.** SDL2 and SDL3 with an ogles3 backend: SDL's 2D renderer moves onto the GPU,
  and SDL programs ask for OpenGL ES 2.0, 3.0 or 3.1. A developer archive ships them prebuilt
  ([`docs/SDL.md`](docs/SDL.md)).
- **Easy to ship.** No installation needed: a program can carry `ogles3.library` and the shader
  compiler in its own drawer. Or install them once with the included installer.

## What it opens up

Most graphics software written in the last fifteen years targets OpenGL ES or something it maps
onto: game engines, emulators with shader-based video, SDL games, and the GPU paths of large
applications such as web-browser engines. OpenGL ES 3.x adds what newer engines expect: instanced
drawing, uniform buffers, multiple render targets for deferred shading, and compute. With ogles3
the graphics side of such a port to the classic Amiga is solved; what remains is the CPU and OS
code.

## Getting it

`release/build_release.sh` builds the archives (`sdl` adds the SDL archive):

| Archive | For | Contents |
|---|---|---|
| `ogles3-1.0-beta1.lha` | users | the library, the shader compiler, `Install-ogles3`, `OGLES3Log`, six examples |
| `ogles3-1.0-beta1-sdk.lha` | Amiga programmers | the same plus the SDK: headers, import libraries for GCC and vbcc, the autodoc `ogles3.doc`, the guide `GettingStarted.txt`, example sources |
| `ogles3-1.0-beta1-sdk.zip` | cross-development on a PC | the SDK alone |
| `ogles3-1.0-beta1-sdl.lha` / `.zip` | SDL programmers | SDL2 and SDL3 with the ogles3 backend, the MiniGL SDK SDL links, two examples; unpacks over the SDK |

The current archives are in [`release/dist/`](release/dist/). ogles2.library 1.3's are in [its repository](https://github.com/ladislawv/ogles2).

### Requirements

| | |
|---|---|
| Hardware | An Amiga with a PiStorm and a Raspberry Pi 4B or CM4 |
| CPU emulation | Emu68 (tested: 1.1.0-beta.1) |
| OS | AmigaOS 3.x with Picasso96 or CyberGraphX (tested on 3.2) |
| Display | A 32-bit RTG screen |

### Beside ogles2.library

ogles3 shares no file with ogles2.library 1.x: its shader compiler is `LIBS:v3dglslc3` (ogles2's
`LIBS:v3dglslc`), its cache `OGLES3Cache:`, its settings `ENV:OGLES3/`, its log `T:ogles3.log`.
Source written for ogles2 builds against the ogles3 SDK with its `#include` lines changed: the
`OGLES2_` names are still defined.

## Writing a program

```c
#include <proto/ogles3.h>        /* OpenGL ES 3.1 and agl*, through the library */
#include <GLES3/gl31.h>          /* the ES 3.x enumerants */

if (OGLES3Open()) {              /* libogles3.a: opens ogles3.library */
    ULONG err;
    void *ctx = aglCreateContextTags2(&err, OGLES3_CCT_DEPTH, 24, TAG_DONE); /* ES 3.1 */
    aglMakeCurrent(ctx);
    /* ... GL calls, aglSwapBuffers() per frame ... */
    aglDestroyContext(ctx);
    OGLES3Close();
}
```

Link with `libogles3.a` (GCC) or `ogles3.lib` (vbcc). `OGLES3_CCT_PISTORM_ES_VERSION` 20 asks for
an OpenGL ES 2.0 context instead (30 and 31 give 3.1). Without a window or screen tag the library
opens its own window; `OGLES3_CCT_WINDOW`, `OGLES3_CCT_MODEID` and `OGLES3_CCT_BITMAP` render into
yours (`include/libraries/ogles3.h`). AmigaOS 4 code compiles unchanged (recompile: the tag values
differ). With clib2 and `-m68881`, link maths with `-lm881`, not `-lm`
([CONTRIBUTING.md](CONTRIBUTING.md#traps)). The SDK's `GettingStarted.txt` walks through windows,
full screen, resizing, shaders, performance, shipping and moving from ogles2, and includes a
prompt for working with an AI assistant.

**Shaders.** With the compiler installed (`LIBS:v3dglslc3`, or in the program's drawer) nothing
is needed: programs compile when they link (about 0.2 s, once), into `OGLES3Cache:` if that
assign exists, else `T:`; `glCompileShader` reports GLSL errors with the compiler's messages. A
draw that needs another variant of a program (for float or integer targets, for instance) gets it
compiled at that draw, and cached too. To ship binaries instead: run the program once and ship
the `.v3dp` files from the cache in a `shaders/` drawer beside it (`PROGDIR:shaders/`), or right
next to it (`PROGDIR:`). Without a compiler, the library writes the sources it is missing to
`T:ogles3_missing_shaders.txt` for the offline compiler on a PC (`tools/v3dglslc`,
`v3dp/split_shaders.py`, `v3dp/v3dp_hashname.py`).

**Logging.** The library logs to `T:ogles3.log`, or to the file named in `ENV:OGLES3/LogFile`;
`OGLES3Log` writes the buffered log while a program is still running. With `KHR_debug` a program
gets every GL error as a message, in a log or through its own callback.

The full reference, with every call, tag, extension, limit and the deviations, is
[`release/ogles3.doc`](release/ogles3.doc).

## Building from source

Everything builds on Linux (Ubuntu 24.04; on Windows inside WSL). The m68k toolchains, Mesa (for
the shader compiler) and the host tools are set up by the scripts in
[`dev/host-setup/`](dev/host-setup/README.md), pinned to the commits ogles3 was tested with. A
working copy's `local.env` names its build directories (`~/ogles3-build`) and its Mesa tree.

```sh
. ./local.env
make -j8 B=$B                              # ~/ogles3-build/m68k: ogles3.library, libogles3.a, tests, OGLES3Log
make -j8 ES3=1 B=~/ogles3-build/m68k-es3   # a test build whose highest context is ES 3.0 (dEQP-GLES3)
make amictl-test                           # host test of the remote test station (no Amiga needed)
bash tests/a1200/build_package.sh m8-5     # an A1200 test package -> tests/a1200/out/ogles2-m8-5.lha

# The shader compiler (needs the Mesa build with tools/v3dglslc/patches applied)
python3 tools/v3dglslc/replay.py           # Mesa for 68k (GCC 16.2), ~900 files
OUT=$V bash tools/v3dglslc/build_m68k.sh   # -> ~/ogles3-build/v3dglslc/v3dglslc
OUT=$V bash tools/v3dglslc/build_host.sh   # -> ~/ogles3-build/v3dglslc/host/v3dglslc (the offline compiler)
OUT=$V bash tools/v3dglslc/test/compare_v3dp.sh   # x86-64 and 68k .v3dp files must be identical

bash release/build_release.sh sdl          # the release archives, into ~/ogles3-build/release
```

The library is built with GCC 6.5.0b (the compiler PiStorm3D is built with), the shader compiler
with GCC 16.2 and software floating point. GCC 16.1 miscompiles and must not be used.

## Testing

Every change is tested on a real A1200, either by hand (copy a package from `tests/a1200/out/`,
run its `Run-*` script, send back `results/`) or remotely through the test station in
`tools/amictl` (a small server on the Amiga, reached over the LAN with a token and an address
allow list). Each package's `ReadMe.txt` says what it does and what to look for; the results of
every run are in `tests/a1200/results/`. The dEQP conformance tests run watched from the PC
(`ports/deqp/run_pc.sh`, `ports/deqp/run_full.sh` for whole lists, `tools/amictl/deqp_watch.py`), with the shaders compiled on the PC.
Real programs are checked against Mesa's reference images (`tests/corpus`).
[CONTRIBUTING.md](CONTRIBUTING.md) describes the workflow, and
[the OpenGL ES 3 plan](docs/GLES3_PLAN.md) the work and its state.

## Layout

| Path | What |
|---|---|
| `ogles2/` | The library's source: contexts, state, programs, uniforms, draws, compute, textures, buffers, framebuffers, jobs, display, KHR_debug |
| `v3dcore/` | PiStorm3D's V3D layer (control lists, memory, textures, submission), see `v3dcore/PROVENANCE.md` |
| `client/` | `libogles3.a`, the import library: `OGLES3Open/Close`, current context, `agl*` |
| `include/` | Public headers: `libraries/ogles3.h`, `clib/ogles3_protos.h`; Khronos `GLES2/`, `GLES3/` and `KHR/` |
| `gen/` | `gen_ogles2.py`: dispatch table, client wrappers, `aglGetProcAddress` thunks, stubs and ABI test, from Khronos `gl.xml` and the append-only `abi_order.txt` |
| `v3dp/` | The `.v3dp` program-binary format (`FORMAT.md`), writer, reader, helpers |
| `tools/v3dglslc/` | The GLSL compiler: Mesa's GL front end and V3D driver on a fake DRM device, built for the PC (offline) and for 68k (on the Amiga), with its Mesa patches |
| `tools/amictl/` | The remote test station: `amictld` (Amiga server), `amictl` (PC client), the dEQP watcher and the compile bridge |
| `ports/` | SDL2, SDL3, dEQP (GLES2, GLES3, GLES31); gl4es, Quake III and glmark2 (still on ogles2's names) |
| `tests/` | Unit and hardware tests, `a1200/` test packages and results, `corpus/` (real programs against reference images) |
| `cmd/` | `OGLES3Log` |
| `dev/` | Build-machine setup (`host-setup/`), the reference sources fetcher, the compiler's feasibility study (`spike-c/`) |
| `release/` | The release builder, ReadMe, installer, autodoc, guide, examples, and the current archives (`dist/`) |
| `docs/` | The development plans (ogles2's, OpenGL ES 3's), the SDL guide |
| `licences/` | Licence texts and third-party notices |

## Licence

`ogles3.library` as built incorporates PiStorm3D's V3D backend (`v3dcore/`), so it is under the
Hyperion MiniGL Open Source License ([`LICENSE`](LICENSE), `licences/License_MiniGL_Library.txt`):
the library may only be built for AmigaOS, and the source of all modifications must be published
to the Amiga developer community. That licence covers `v3dcore/`, the six front-end files that
adapt PiStorm3D's code (listed in `LICENSE`) and `tools/builtin_shaders/`. Everything else written
for this project is MIT, including the import library and headers programs build against
(`client/`, `include/libraries/`, `include/clib/`), so a program of any licence, GPL included, may
link them. Third-party code and the ports keep their own licences and notices (`LICENSE` sections
3 and 4; `licences/License_V3D_ThirdParty.txt`).

## Credits

ogles3 is developed by R-Typer and Ladislawv, with Claude Opus 5.5 (Anthropic's AI assistant).

It builds on: PiStorm3D (`minigl.library` for PiStorm) by Steffen "MagicSN" Haeuser, and its V3D
backend, which this library builds on, by Dennis van der Boon; the V3D assembler and V3D
documentation by Macoy Madson; Mesa's V3D driver and compiler (Broadcom, VMware and the Mesa
contributors); Emu68 by Michal Schulz; PiStorm by Claude Schwarz and contributors; bebbo's
amiga-gcc; SDL2_68k and SDL3_68k by Steffen Haeuser; the Khronos dEQP test suite.
