# OpenGL ES 3.0 and 3.1 on ogles2.library

**Status: plan (2026-10-18; revised eight times, research added and checked
2026-10-19, every line citation and each stage reviewed for implementation
the same day), work on branch `gles3`.** Stage A (begun 2026-10-22) is
done, tested on the A1200 (§4, each item's "Done" paragraph and "Stage A's
end", 2026-10-23): the GLES2 regression set and the speed figures as at
1.3.1; dEQP-GLES2's whole list is postponed until the ES3 library is
finished. Stages B-D are still to come; §1 "Where to go on" is the
hand-off.
Its facts were checked against this repository, Mesa at the pinned commit
(`~/mesa-build/mesa`), Khronos' `gl.xml` (`gen/khronos/`) and extension
specifications, the Linux kernel's V3D driver (`drivers/gpu/drm/v3d`,
kernel.org, 2026-10-18), VK-GL-CTS `opengl-es-cts-3.2.14.1` (`~/vk-gl-cts`,
the source of the dEQP port), the sources of the ports (gl4es, SDL, Quake
III) and the host `v3dglslc`. Appendix B holds the research each stage
needs, with its sources; the few facts not confirmed are marked
"unverified". "The main plan" is `docs/DEVELOPMENT_PLAN.md`.

ogles2.library 1.3 is a complete OpenGL ES 2.0 with 33 extensions. The GPU it
drives, the Raspberry Pi 4's V3D 4.2, is OpenGL ES 3.1 capable: Mesa's V3D
driver, which `v3dglslc` is built from, is conformant for GLES 3.1 on it
under Linux. This plan extends ogles2 to OpenGL ES 3.0, then 3.1, as far as
the hardware allows, without changing anything for GLES2 programs.

Not in scope: tessellation and the rest of OpenGL ES 3.2. The packet
definitions have a tessellation shader state record
(`mesa/src/broadcom/cle/v3d_packet.xml:1389`), but neither of Mesa's V3D
drivers supports tessellation shaders (the Vulkan one reports
`tessellationShader = false`, `mesa/src/broadcom/vulkan/v3dv_device.c:329`),
so there is no compiler for them. Geometry shaders exist on V3D 4.2 and in
Mesa's driver, so they could follow later as an extension
(`GL_EXT_geometry_shader`, §10).

## 1. Where the work lives, and releases

- **Private until released.** The branch `gles3` lives in the private
  repository `github.com/ladislawv/ogles3.1`, in a working copy of its own
  beside the ogles2 clone; the public `github.com/ladislawv/ogles2` is a
  fetch-only remote there (`ogles2`), and a `pre-push` hook refuses `gles3`
  to it. `local.env` (not in git) sends this copy's builds to
  `~/ogles3-build` and names its own Mesa tree (`MESA`, `MESA_INSTALL`;
  §3). The compiler, test and release scripts default `OUT` to
  `~/ogles2-build` (`tools/v3dglslc/build_host.sh:8`, `build_m68k.sh:6`,
  `release/build_release.sh:28`), so `OUT` is given per command, as
  `local.env` says, or a 1.x build is overwritten. Another machine is set
  up with `setup_gles3.sh` from `gles3-setup.zip` (from the developer; not
  in the repository).
- **Working on it** (the commands as of 2026-10-22; in WSL, in this copy):
  - **Set-up:** `setup_gles3.sh` (above), then `. ./local.env` before every
    build: it sets `B`, `BS`, `V`, `V3DGLSLC` (`~/ogles3-build`) and `MESA`,
    `MESA_INSTALL` (this work's Mesa tree, §3). A new machine builds that
    tree with `build_mesa.sh`, `apply_mesa_patches.sh` and
    `ninja -C $MESA/build install` after `. ./local.env` (on WSL1 also
    `patch_drmshim.sh`, which takes `$MESA` since 2026-10-23: before, it
    patched and rebuilt the 1.x tree whatever `local.env` said). Cloning
    `~/mesa-build/mesa` into `$MESA` first (`git clone`, the 1.x tree's
    patches stay out: they are not committed) saves the download.
  - **Trap: the Mesa trees.** This copy's `tools/v3dglslc/patches` has
    0015 on, which the 1.x tree (`~/mesa-build/mesa`) must never get, and
    `apply_mesa_patches.sh` without an argument works on `$MESA`, or on the
    1.x tree when `MESA` is unset. So from this copy run it only after
    `. ./local.env`, or with the tree named; check another tree with
    `git apply --check [--reverse]`, never with the script (it applies).
    (Found 2026-10-22: a script without `local.env` put 0015 into the 1.x
    tree; taken out again, that tree not rebuilt meanwhile.)
  - **Builds:** `make -j8` (as a release builds the library: ES 3.0
    contexts refused) and `make -j8 ES3=1 B=~/ogles3-build/m68k-es3` (the
    test build with ES 3.0 contexts); the compiler with
    `OUT=$V bash tools/v3dglslc/build_host.sh`, and for the m68k one
    `OUT=$V python3 tools/v3dglslc/replay.py` then
    `OUT=$V bash tools/v3dglslc/build_m68k.sh`.
  - **Checks on the PC, after every change:**
    `OUT=$V bash tools/v3dglslc/test/compare_v3dp.sh` (host and m68k
    compilers byte for byte, the ES3 test programs included; every file
    decoded by `v3dp_dump.py`); `make es3-header-probe`;
    `B=... bash tests/vamos.sh m8_es3 NOGPU` with the release build and
    `... m8_es3 NOGPU ES3` with the test build (vamos runs state-only
    tests: no GPU, no FPU; `make test-v3dp` first, for the programs'
    files, which `vamos.sh` puts in `T:`: under vamos `PROGDIR:` is not
    the test's drawer and assigns are missing from the DOS list, so the
    library's cache is `T:`); `B=~/ogles3-build/m68k-es3 bash
    tests/m8_es3/check_limits.sh` (the library's limits against
    `v3dglslc -L`); `B=$B bash tests/vamos.sh m2_link NOGPU` (GLES2
    programs linked from their files, 27 checks); after a compiler or
    writer change `python3 tools/v3dglslc/test/variant_roundtrip.py
    $V3DGLSLC <dir> <vs> <fs>` for each test program (every variant
    compiled again from its key alone, byte for byte, and each fragment
    variant with its vertex variant in one request: the draw-time
    variants' ground); after a change to the keys (`ogles2/variant_key.c`)
    or the compiler `make variant-key-test` (the library's keys against
    Mesa's, 486 checks); and after a Mesa change the m68k compiler again
    (`replay.py`, `build_m68k.sh`: the Mesa objects are replayed from the
    tree). GLES2 programs' files must stay byte for byte what the
    compiler wrote before (keep a compiler built before a change and
    compare its output on `tools/v3dglslc/test/shaders`). The smoke test
    passes again: its two checks of an error set through the table used
    a stub, which no entry is any more (now a bad target's
    GL_INVALID_ENUM).
  - **On the A1200:** check `List LIBS:ogles2#? LIBS:v3dglslc` is empty
    first (`amictl.py run`), then package `m8-1` (`bash
    tests/a1200/build_package.sh m8-1` after `. ./local.env`, then
    `python3 tools/amictl/amictl.py runpkg tests/a1200/out/ogles2-m8-1 -t
    900`), seven steps: `smoke` and `m8_es3` with the release build;
    `m8_es3 ES3`, `LIMITS` and `m8_draw` with the test build and the
    m68k compiler in `LIBS:` (draw-time variants compiled into the run's
    `cache/`, a 1.x compiler's file there replaced); `m8_draw` again
    without the compiler (from the cache), then with the cache's `.v3dx`
    files beside the program. Last passed 2026-10-23 (`tests/a1200/results/
    ogles2-m8-1-20261023-143820`: 14, 42, 42 limits identical, 17, 17, 17;
    the package needs a 1.x compiler in `~/ogles2-build/v3dglslc`, `V1`). A
    GLES2 drawing regression after any change to the draw, the reader or
    the textures: package `m7-4` from this copy's sync build (`B=$BS V=$V
    bash tests/a1200/build_package.sh m7-4`; 23, 44, 33 and 29 checks as
    at 1.3, `ogles2-m7-4-20261023-143829`). Results directories are
    committed (`tests/a1200/results/`), screenshots never.
  - **dEQP-GLES3** (item 6): `MODULE=gles3 B=~/ogles3-build/m68k-es3 bash
    ports/deqp/build.sh`, package `m8-2` (`bash tests/a1200/build_package.sh
    m8-2` after `. ./local.env`), groups with `python ports/deqp/run_groups.py
    --package=m8-2 <group>...` (patch `gles3/0001`, which let a run go on
    past the state reset stage A's library refused, is gone since
    2026-10-25: §5 item 13).
  - **The GLES2 regression set** (§3): the packages take `B`, `BS`, `V`
    from `local.env`, except `m5-1`, `m6-6`, `m6-8`, `m6-10`, `m7-2` and
    `m7-3`, whose `B` is the sync build (`B=$BS V=$V bash
    tests/a1200/build_package.sh <name>`). The corpus is judged on the PC:
    `python3 tests/corpus/compare.py ~/ogles3-build/m68k/corpus/ref
    <results>/results`. Speed against 1.x needs 1.x's binaries
    (`~/ogles2-build/m68k/ioq3gl2`, `glmark2`), §4 "Stage A's end".
    After a crash on the A1200 the library stays in memory, open (`Version
    ogles2.library` answers though `LIBS:` has none): reboot, with the
    developer's leave, before the next run.
  - **Where to go on, in order** (hand-off of 2026-10-23: stage A is done
    but for dEQP-GLES2, §4 "Stage A's end"; 2026-10-24: dEQP-GLES3's
    `builtin_variable.fragcoord_xyz` and `fragcoord_w` pass, 2 of 2,
    `ogles2-m8-2-20261024-223221`, as do the four groups not run before,
    `shaders-declarations`, `-qualification_order`, `-linkage` and
    `-switch`, 425 of 425, `ogles2-m8-2-20261024-223828` to `-223938`.
    **Trap:** the A1200 then had a 1.x release installed, `LIBS:ogles2.library`
    and `LIBS:v3dglslc` (no `$VER:`); the library takes `LIBS:v3dglslc`
    before a package's compiler, and the first runs failed 222 cases on
    its format 1.0 files (refused, as they must be). Check
    `List LIBS:ogles2#? LIBS:v3dglslc#?`, and with the owner's consent
    rename both to `.saved` for the runs and back afterwards): (1) stage
    B (§5). Item 13's state is done (2026-10-25): `glu::resetState` raises
    no error in an ES 3.0 context, and patch `gles3/0001` is gone (its
    parts, in `framework/opengl/gluStateReset.cpp`'s order: primitive
    restart and rasterizer discard, §5 item 12; swizzle, compare mode,
    sampler objects, 2D array and 3D textures and the pixel store, items 7
    and 8; the buffer targets and indexed bindings, item 2; the transform
    feedback and query state, item 12). Without it the stage A groups pass
    as with it (`info` 6, `shaders-fragdata` 8, `-function` 4,
    `-negative` 12, `-discard` 25, `-return` 38, `-conditionals` 41,
    `-declarations` 54, `-qualification_order` 79, `-linkage` 118, and
    `shaders-builtin_variable` 7 of 29: the 22 of the shader executor's
    sized formats, item 10), `switch` 174, and dEQP confirms the new
    features: `primitive_restart` 504 of 504, `samplers` 42 of 42
    (`ogles2-m8-2-20261025-002234` to `-002732`); `pbo` 0 of 54 waits for
    sized renderbuffers (item 10), `glMapBufferRange` (item 12, done since)
    and ES 3.0's read formats. A run's `.txt` lists the ES 3.0 entries the
    library lacks (the platform's no-op stubs): a work list. Then the
    rest of stage B: done since are uniform buffers (items 1 and 6:
    `shaders-uniform_block` 138 of 138; `ubo` stopped the A1200 in its
    1388th case, item 6's open point), buffer mapping (item 12) and
    vertex array objects in ES3 contexts (item 3): `buffer` 695 of 695,
    `vertex_array_objects` 10 of 10; since then instancing (item 4), the
    vertex formats (item 5), sync objects and occlusion queries (item 12),
    `glDrawBuffers` and `glReadBuffer` (item 11): `instanced` 45 of 45,
    `vertex_arrays` 1005 of 1005, `fence_sync` 10, `occlusion_query` 62;
    since then item 8's textures (immutable storage, every ES 3.0 sized
    format, integer samplers: `texture` 4101 of 5895, no failure) and
    item 9's ETC2/EAC (ETC1 sampled natively too): every ETC1, ETC2 and EAC
    case of dEQP-GLES3 (492) and dEQP-GLES2's 97 ETC1 cases pass; since
    then item 10's render targets beyond 8 bits (`fragment_out` 940 of
    940, `read_pixels` 48, `pbo` 54, `color_clear` 19,
    `shaders-builtin_variable` 29 of 29, `fragment_ops` 3165 of 3174,
    `texture` 4501 of 5895 and no failure). **Trap found with it:** a
    colour write of the wrong width into a 32-bit float or integer target
    leaves the A1200 drawing wrong pixels until it is switched off (§5 item
    10; CONTRIBUTING.md, "Traps"): after a GPU-heavy run, `m7-4` passing is
    the check that the machine is sound. Since then most of item 11
    (the draw and read bindings, `GL_DEPTH_STENCIL_ATTACHMENT`, layers,
    invalidation, ES 3.0's completeness and queries, colour blits), then
    its depth and stencil blits and `DEPTH32F_STENCIL8` textures' stencil:
    `fbo` 2032 of 2075 and none failing, `fragment_ops` 3174 of 3174.
    Next: item 8's rest (glCopyTexSubImage3D), then item 12's.
    **Trap:**
    `ports/deqp/build.sh` copies the CTS with `rsync -a`, which keeps the
    sources' old dates, so a patch changed or removed rebuilds nothing:
    delete the copy (`$B/deqp-gles3/src`), and for a removed patch the
    objects of the files it touched (`ports/deqp/README.md`). dEQP-GLES2's
    whole list when the ES3 library is finished. §4's items each have a "Done"
    paragraph that says what is in and where; the code of this work so far
    is in `context.c` (versions, extensions), `state.c` (limits),
    `program.c` (the `#version` check, uniforms, `glGetFragDataLocation`,
    the lookup, `.v3dx` images), `v3dp.c` (format 2), `draw.c` (blocks,
    `textureSize`, varying flags, `gl_VertexID`, `es3_variants`),
    `variant_key.c` (the keys),
    `texture.c` (`OGLES2_TEX_NONE`, `ogles2_texture_size`),
    `compiler.c` (`V3DGLSL_COMPILE_VARIANT`), and on the compiler's side
    `v3dglslc.c` (`-L`, `-x`, the key-state draws, the server),
    `v3dp_writer.c` (format 2, `.v3dx`, `v3dp_key_parse`) and Mesa patch
    0015.
- **`main`** (public) is the released 1.x line. Fixes and small additions go
  there and are released as 1.4, 1.5 and so on (the library's revision; the
  dispatch table only grows, the ABI version stays).
- **1.x fixes first, on `main` (1.4):**
  - **GLES2 contexts refuse GLSL ES 3.x shaders.** 1.3 does not:
    `glCompileShader` does not look at `#version`, and the compiler's Mesa
    context is already ES 3.1 (§2), so a `#version 300 es` program compiles
    and links, and may draw, without its `flat` qualifiers (§2) and with
    its file in format 1.0. The library refuses `#version 300 es` and
    `310 es` in the compile log ("GLSL ES 3.00 needs an OpenGL ES 3
    context"), as the GLSL ES 1.00 specification requires of a version the
    implementation does not support. The check is the library's own (it
    reads the `#version` line), so it also works without the compiler, and
    it comes before the shader record lookup (`.v3ds`).
  - **`OGLES2_VERSION`.** `ogles2.doc` (`:242`) and
    `<libraries/ogles2.h>` (`:21-25`) say a program may define it on its
    compile line to require a newer library. That has no effect:
    `OGLES2Open()` is compiled into `libogles2.a` with the header's value 1
    (`client/ogles2_open.c:52`). What does work is the dispatch-size check
    (§3). 1.4 corrects both texts.
  - **`FORMAT.md`'s hash order.** It says the shaders are hashed "in attach
    order"; the library and the compiler hash the vertex shader, then the
    fragment shader, then the bindings (`ogles2/program.c:683-685`,
    `tools/v3dglslc/v3dglslc.c:377-379`). 1.4 corrects `FORMAT.md`, and with it
    the table row that a line break inside `<index> <name>` splits
    (`FORMAT.md:75-76`).
  - **`.v3dw` and `.v3da` of a program linked with its bindings.** Their
    requests carry no bindings (`compiler.c:290`, `:299`), and the compiler
    reads them only for `V3DGLSL_LINK_BOUND` (`v3dglslc.c:502`), so it links
    such a program without them and hashes two sources: the file's hash
    differs from the program's, and the library drops the file without a
    word (`program.c:968`). 1.4 adds request codes that carry the bindings
    (an older library's request is shorter, so the old codes cannot read
    them); stage A's `V3DGLSL_COMPILE_VARIANT` sends them in any case.
- **`gles3` takes in `main`** after each `main` release at least
  (`git fetch ogles2 && git merge ogles2/main`), so 1.x fixes reach the ES3
  code and the GLES2 regression tests on `gles3` test today's 1.x code.
- **Numbers are `main`'s first.** Whatever gets a number on `gles3` keeps it
  only once a release publishes it: dispatch entries (`gen/abi_order.txt`),
  context tags and their values, compiler request codes (`V3DGLSL_*`) and
  the fields appended to `struct V3DGLSLRequest`, `.v3dp` section types,
  stage values and feature bits, Mesa patch numbers, test package names.
  Until then each merge of `main` puts `gles3`'s new ones after `main`'s
  (`abi_order.txt` keeps `main`'s order, then `gles3`'s entries; the rest
  renumbered to match). Nothing built from `gles3` is given out before its
  release, so no program depends on a number that moves.
  `V3DGLSL_VERSION` stays 1 on both branches: the compiler refuses a
  request of any other version (`v3dglslc.c:474`). `gen/abi_order.txt` is
  append-only for the generator, which never reorders it; such a merge is
  the one place where the file is edited by hand. The host checks
  (`.github/workflows/host-checks.yml`, on every push) run in the private
  repository too and check that the file is complete.
- **Releases from this work** (decided at each merge, §10):
  - **2.0, after stage B:** stage B's features as **GLES2 extensions** in
    GLES2 contexts: `OES_vertex_array_object`, `EXT_instanced_arrays`,
    `OES_texture_3D`, `EXT_texture_storage`, `EXT_texture_rg`, `EXT_sRGB`,
    `EXT_map_buffer_range` (the GLES2 extensions of ES 3.0 features stage B
    builds; all in Khronos' registry, all ES 2.0 ones, as Mesa's
    `extensions_table.h` agrees; `EXT_color_buffer_half_float` is in 1.3 since
    GitHub issue 5, `main` 7f4b91d), and `OES_depth24` and `EXT_frag_depth`:
    24-bit depth renderbuffers come with stage B, and the compiler and the
    draw already handle depth writes (tested 2026-10-19: `gl_FragDepthEXT`
    compiles; `draw.c:863`). Of the programs here, gl4es looks for
    `EXT_texture_rg`, `OES_depth24` and `EXT_frag_depth`
    (`gl4es/src/glx/hardext.c:327-377`; with its `floattex` option also for
    `EXT_color_buffer_half_float`, which 1.3 has); SDL3 looks for none of
    these (only for `EXT_sRGB_write_control`, an ES 3.0 extension, §10); Quake
    III's GLES2 renderer looks for `EXT_occlusion_query_boolean`,
    `EXT_shadow_samplers`, `NV_read_depth`, `NV_read_stencil`,
    `OES_standard_derivatives` and `OES_element_index_uint` (its GLES branch,
    `code/renderergl2/tr_extensions.c:70-163`, taken because
    `ports/ioq3-gl2/amiga_glimp_gl2.c:186` reads "OpenGL ES" from
    `GL_VERSION`), so 2.0, which offers none of the four that 1.3 lacks,
    changes nothing for it (§10). ETC2/EAC has no GLES2 extension: it comes
    with the ES3 contexts (2.1), while ETC1 is sampled natively at once (§5
    item 9). Not in 2.0: `OES_texture_stencil8`, which needs ES 3.1 (2.2;
    gl4es looks for it, but in the GLES2 contexts it creates, which never list
    it). No ES3 context yet: an ES 3.0 context must offer all of ES 3.0, and
    transform feedback, multisampled framebuffer objects and pixel buffer
    objects come only in stage C. Release 2.0 writes no format 2 file (§3).
  - **2.1, after stage C:** ES 3.0 contexts, dEQP-GLES3 passing.
  - **2.2, after stage D:** ES 3.1.

  Each release: `gles3` merged into `main` in a clone of the public
  repository, `main` pushed; from then on that history is public.
- Tags as now: `v1.4`, ..., `v2.0`. The release archives (`release/dist/`)
  are refreshed on `main` only.

## 2. What exists

- **GPU:** V3D 4.2: compute shader dispatch (the CSD unit), cache flush and
  clean for shader writes, the texture formatting unit (TFU), instanced and
  indirect draws in the control list, occlusion counters, transform
  feedback, multisampled tiles, 16- and 32-bit float and integer render
  targets, sRGB conversion at the tile load and store, ETC2/EAC and BC1-BC3
  textures in the TMU, `textureGather`, seamless cube map filtering (always
  on). Appendix B.3 and B.4 give the packets and registers.
- **Limits** (Mesa's `v3d_limits.h`, `v3d_screen.c`, `u_screen.c`), each at
  or above the ES 3.0 and 3.1 minimum (in brackets): 4096 texels a side
  (2048; 13 levels, also for 3D: 256), 2048 array layers (256), texel offsets
  -8 to 7 (the same), vertex attribute strides up to 2048 (2048), 4 render
  targets (4), 4 samples (4), 64 vertex output and fragment input components
  (64, 60), 16 constant buffers a stage, the default one included (12
  uniform blocks), 64 KB each (16 KB), 4 transform feedback buffers (4),
  compute work groups of up to 256 invocations (128) and 65535 a dimension
  (65535), 32 KB compute shared memory (16 KB), storage blocks of 128 MB
  (128 MB), 16 vertex attribute bindings and relative offsets up to 2047
  (16, 2047), uniform buffer offsets aligned to 256 bytes (at most 256),
  storage buffer offsets to 4. Storage buffers, images and atomic
  counters: fragment and compute shaders only (Mesa offers images in the
  vertex stage too, `drv/v3d_screen.c:172-178`, but the vertex shader runs
  for binning and again for each tile it touches, so its stores would
  repeat); ES 3.1 allows 0 of each in the vertex stage. Element indices: Mesa
  writes 0xffffff into the attribute record's 32-bit maximum index field
  (`v3dx_draw.c:844`) and reports its core default 2^32 - 1; ogles2 will
  report 2^24 - 1, ES 3.0's minimum (stage A; 1.3 has no
  `GL_MAX_ELEMENT_INDEX`).
- **Limits ES 3.1 raises above 1.3's values** (dEQP-GLES31's
  `es31fIntegerStateQueryTests.cpp:680-836` checks them): 48 combined
  texture units (ES 3.0: 32; 1.3 has 32), 36 uniform buffer bindings (ES
  3.0: 24). The units are binding points of the library (16 a stage, three
  stages with compute), so the library gets 48 **in every context from
  2.0**: `gl_MaxCombinedTextureImageUnits` is a GLSL ES 1.00 built-in, and
  one value for both context versions keeps a 1.00 shader's compiled file
  the same in both (GLES2 and ES 3.0 allow more than their minimum).
- **Compiler:** `v3dglslc`'s fake DRM device
  (`tools/v3dglslc/compat/fake_drm.c`) reports CSD and cache flush, so Mesa's
  V3D driver enables compute shaders, storage buffers and images; its
  `SUBMIT_CSD` and `SUBMIT_CL` do nothing, which is all a compile needs. It
  cannot export a sync file: Mesa then logs "MESA: error: Export failed"
  (`drv/v3d_context.c:72`, a flush with a fence), seen with a GLES2 program
  sampling a 3D texture (the file is written all the same). Its one Mesa
  context (`attribs.major = 2`, `v3dglslc.c:587`) is **already ES 3.1**: Mesa
  creates the highest version the driver allows. Tested 2026-10-18 with the
  host compiler: `#version 300 es` and `310 es` programs compile and link
  (with `flat`, `layout(location)`, `gl_VertexID`), `320 es` is refused
  ("Supported versions are: 1.00 ES, 3.00 ES, and 3.10 ES"). `v3dglslc` sets
  ogles2's limits in it (4 draw buffers, 16 texture units a stage, 32 in all),
  so GLSL's `gl_Max*` constants equal the library's queries. It has a build
  stamp (`$V3DGLSLC-STAMP:`, `v3dglslc.c:62`) but no `$VER:` string (one
  since stage A item 2, 2026-10-23).
- **Library:** the GLES 2.0 core, 33 extensions, 164 dispatch entries,
  `GL_VERSION` "OpenGL ES 2.0 ogles2.library 1.3", library version 1
  (`lib_Version`), dispatch ABI 1, 240,312 bytes (release 1.3.1, the same
  library version). Share
  groups (`OGLES2_CCT_SHARE_WITH`, `share.c`) hold textures, buffers, shaders
  and programs, and renderbuffers. Already covering parts of ES 3.0: several
  render targets (`EXT_draw_buffers`), float, half-float and depth textures
  (sampled), half-float colour targets (`EXT_color_buffer_half_float`, GitHub
  issue 5: RGBA16F textures and RGBA16F, RGB16F, RG16F and R16F renderbuffers,
  all stored as RGBA16F), `glCopyTexImage2D` and `glCopyTexSubImage2D` drawn
  by the GPU (`copy.c`), 32-bit index buffers, program binaries, standard
  derivatives, 32 texture units, 4x MSAA on the default framebuffer, base and
  max level and LOD clamps (`SGIS_texture_lod`), the vertex and instance ID
  flags of the shader record (`draw.c`, from prog data's
  `uses_vid`/`uses_iid`), and `glDrawRangeElements` (AmigaOS 4's entry, which
  is ES 3.0's). Unknown context tags are logged and ignored.
- **What the library lacks, besides the API** (Appendix B.2 has the places):
  - **no GPU copy of a buffer object:** a buffer is a CPU copy in the 68k's
    byte order (`buffer.c`), and draws read converted copies per attribute
    layout; uniform buffers, transform feedback, storage buffers and atomic
    counters need the GPU to read or write the buffer itself;
  - **buffers:** only the `GL_ARRAY_BUFFER` and `GL_ELEMENT_ARRAY_BUFFER`
    targets and the three `*_DRAW` usages (`buffer.c:256-500`); a deleted
    buffer is unbound from every share member's current bindings and freed
    at once (`buffer.c:215-237`), with no references from other objects;
  - **no `flat` or `centroid` varyings:** the reader reads the flags, but
    `draw.c` sends only the "zero all" packets;
  - **vertex numbering:** draws start the attribute copies at `first` and
    draw from vertex 0, and indexed draws rebase their indices
    (`draw.c:1654`, `:1601-1625`), so a vertex's index is not the one ES
    3.0 gives `gl_VertexID` (GLSL ES 1.00 has no `gl_VertexID`); 32-bit
    indices of 2^31 or more are refused, so a restart index would be
    rebased, refused or widen the copied vertex range;
  - **vertex attributes:** float, fixed, short and byte only; no divisor, no
    integer reads; a disabled array reads a float record
    (`constant_attrib`, `draw.c:794-808`) and missing components come from
    one (0, 0, 0, 1.0) block per control list (`default_attribs`,
    `:608-624`); matrix attributes get columns only for `mat2`, `mat3` and
    `mat4` (`v3dp.c:344-345`);
  - **uniforms:** `glUniformMatrix*fv` refuse `transpose` (GLES 2.0's rule,
    `program.c:1789-1817`);
  - **one framebuffer binding** (`ctx->draw_fb`), no separate read binding;
    attachment queries refuse the default framebuffer and answer
    `GL_INVALID_ENUM` on an empty attachment (`fbo.c:1299-1311`);
  - **framebuffer objects:** each target 32 bpp of internal type 8 or, for
    a half-float one, 64 bpp of type 16F, clamped never
    (`TileRenderingModeCFGColorEach`, `job.c:602-627`), the clear colour in
    parts 1 and 2 (`clear_colors`, `:482-501`); never multisampled; tiles
    and pools follow Mesa for 32- and 64-bit targets, down to 32x16
    (`fbo.c:1097-1116`, the pools at `:1035-1077`); no 128-bit, integer or sRGB
    targets, no part 3 clear colour but for a UIF pad; one of size 0 cannot
    be drawn (`fbo.c:1117`, `job.c:848-850`); renderbuffers:
    `DEPTH_COMPONENT16`, `STENCIL_INDEX8`, `DEPTH24_STENCIL8`, the colour
    formats of GLES2 and the four half-float ones (`fbo.c:166-190`), no
    `DEPTH_COMPONENT24`, which ES 3.0 requires; `glReadPixels` reads a
    half-float target as clamped bytes only (`read_half`,
    `texture.c:1841`; a deviation in `ogles2.doc`, `:193-195`);
  - **blending:** one blend state and one `BLEND_CFG` for all render targets
    (`BLEND_ENABLES 0x0F`, `draw.c:1266-1272`);
  - **clear and blend colours** are clamped to 0-1 when set
    (`state.c:157-160`, `:435`); clears the tile buffer cannot do
    (scissored, masked, mid-frame) and `glCopyTex*` on the GPU draw with
    built-in programs (`draw.c`'s built-in image, `clear_mrt_v3dp.h`,
    `copy_tex_v3dp.h`), whose variants serve 8-bit and 16F targets alike
    (Mesa's key does not change for 16F);
  - **textures:** texture and sampler records are cached in the texture and
    reused while its parameters are unchanged (`texture.c:2765-2771`);
    completeness measures the mip chain from level 0 (`complete`,
    `:1152-1170`); 32-bit float and depth textures with linear filters are
    sampled nearest, a GLES2 deviation (`:2846`); ES 3.0 makes such a
    texture incomplete instead, except a depth texture with compare mode on
    (§3.8.13 of the specification);
  - **queries** answer at most 8 values (`state.c:544`), which ES3's
    compressed format list (15) exceeds;
  - **jobs:** renders into framebuffer objects are left running while the
    next job is built (`may_defer`, `job.c:749-760`); no cache clean after
    a job (only invalidation before one, `v3d_invalidate_caches`); no CSD
    code or registers;
  - **CPU and GPU memory:** CPU reads of memory the GPU wrote happen only
    for read-backs (`CachePostDMA`, `display.c`, `texture.c`); blocks are
    aligned to 64 bytes at the start but not padded at the end
    (`v3d_mem_allocvec.c:45`, `:81-98`);
  - **no per-context checks:** `aglGetProcAddress` lives in the client
    library and returns every table entry; an entry has to check the
    context version itself;
  - **the compiler is optional:** `GL_SHADER_COMPILER` is always TRUE, and
    without the compiler `glCompileShader` reports success unchecked
    (`state.c:687`, main plan §7.8).
- **Variants today:** the reader keeps of each variant's key only its
  primitive class, render-target count and point size; the draw picks the
  variants by those, and from the program's `.v3dw` (32-bit samplers) or
  `.v3da` (alpha to coverage) file when such a state is on. With no exact
  match it takes the closest variant and logs it once (main plan §7.8).
  `FORMAT.md`'s "the runtime uses a variant only when its key equals the key
  the draw computes" is the format's intent, not what 1.3 does.
- **`.v3dp` format 1.0** (`v3dp/FORMAT.md`): stages vertex, binning and
  fragment; uniform recipe types are Mesa's enum values (0-46). The reader
  (`ogles2/v3dp.c`) refuses an unknown major version, an unknown stage,
  recipe types it does not implement (`recipe_ok`), uniform blocks other than
  the default one, anything but a vertex and fragment program, and more
  than 64 sections (`:169`, `:193`). It accepts any minor version and skips
  unknown sections. The lookup stops at a file it refuses
  (`program.c:926`): it neither tries the next place nor compiles.
- **Tests:** the dEQP-GLES2 port (`ports/deqp`, package `m7-1`; its must-pass
  list `aosp_mustpass/main/gles2-main.txt`, 17,165 cases, about 2.5 hours on
  the A1200), the A1200 test packages, the corpus, Quake III, glmark2. The
  same CTS has `dEQP-GLES3` and `dEQP-GLES31` (`modules/gles3`,
  `modules/gles31`). Test packages add their `libs` after the system's
  (`Assign LIBS: M7R1:libs ADD`, `tests/a1200/m7-1/Run-M1:64`), so a release
  installed in `LIBS:` wins over a package's library and compiler.

## 3. Shared ground

- **One library, two context versions.** A new context tag,
  `OGLES2_CCT_PISTORM_ES_VERSION` (ULONG; name and values provisional,
  §10), asks for a version as major x 10 + minor: 20 (the default), 30 or
  31. 20 gives a GLES2 context; 30 and 31 give the highest ES 3.x the
  library has, at least the one asked for (a request for 30 gets 3.1 from
  2.2, which ES 3.1's compatibility allows); a version the library lacks
  fails with `OGLES2_CCE_UNSUPPORTED`, any other value with
  `OGLES2_CCE_BAD_TAGS`. A
  GLES2 context behaves as in 1.x, apart from the table's added entries
  (`aglGetProcAddress` returns them; the ES3-only ones refuse with
  `GL_INVALID_OPERATION` there) and 2.0's GLES2 extensions: `GL_VERSION`
  stays "OpenGL ES 2.0 ogles2.library 2.x", and GLSL ES 3.x shaders are
  refused (§1's 1.4 fix). An ES3 context reports "OpenGL ES 3.0
  ogles2.library 2.x" (later 3.1), GLSL "OpenGL ES GLSL ES 3.00" (later
  3.10), `GL_MAJOR_VERSION` and `GL_MINOR_VERSION`, and accepts
  `#version 100`, `300 es` and (stage D) `310 es`. One library keeps one
  driver and one SDK.
- **Old libraries and new programs.** 1.x ignores tags it does not know, so
  a program asking for ES 3 there would get a GLES2 context; what keeps
  ES3 programs off old libraries is `OGLES2Open()`'s dispatch-size check
  (`client/ogles2_open.c:60`): a program linked with the 2.1 SDK's
  `libogles2.a` refuses a library whose table is shorter, 2.0 and 1.x
  among them. A program still checks `aglCreateContext2`'s error code:
  2.0 knows the tag and refuses 30 and 31 (`OGLES2_CCE_UNSUPPORTED`), so
  no program sees a half ES 3.0; only test builds (a build option,
  `make ES3=1`) accept them, from stage A.
- **Share groups** (ES 3.0 appendix D): samplers and sync objects join the
  group's name tables beside textures, buffers, shaders and programs, and
  renderbuffers (`struct OGLES2Share`); vertex array objects, queries,
  transform feedback objects and program pipelines stay per context, as
  framebuffers do now. A context may share only with a context of the same
  version (this library's choice: a GLES2 member would see objects it
  cannot use, such as integer textures or uniform buffers). The versions
  the contexts get are compared (from 2.2, 30 and 31 both give 3.1), so
  the tag is read before the group is joined (`ogles2_share_join`,
  `context.c:79`, comes before the tag loop now), and a mismatch fails
  with `OGLES2_CCE_BAD_TAGS`, as an invalid `OGLES2_CCT_SHARE_WITH` does
  (`context.c:58-62`).
- **The compiler is mandatory in ES 3.0** (dEQP-GLES3 expects
  `GL_SHADER_COMPILER` TRUE, `es3fImplementationLimitTests.cpp:546`). An ES3
  context is still created without `v3dglslc`, so programs that ship their
  binaries run; `glCompileShader` then reports success unchecked, as in 1.x,
  and `ogles2.doc` lists it among the ES3 deviations.
- **Entry points** come from Khronos' `gl.xml` as now: each command gets a
  `command` line in `gen/api.txt` when it is implemented, and
  `gen/gen_ogles2.py --append` adds it to `gen/abi_order.txt` (from which the
  generator writes the dispatch header, `<proto/ogles2.h>`, the table, the
  stubs and `aglGetProcAddress`'s thunks and name table, and from them
  `libogles2.a` and vbcc's `ogles2.lib` are built). A
  `feature GL_ES_VERSION_3_0` line would pull in all its commands at once, and
  the generator refuses to run while any is missing from `abi_order.txt`
  (`resolve_order`, `gen_ogles2.py:141-164`; the host checks run it without
  `--append`): so the `feature` lines come last, when every command of the
  version is in. ES 3.0 has 104 commands, 103 of them new to the table
  (`glDrawRangeElements` is there); ES 3.1 68 more. Appendix A gives each
  one's stage. ES 3.0 names get entries of their own where 1.3 has the
  extension's (`glDrawBuffers` beside `glDrawBuffersEXT`, `glUnmapBuffer`
  beside `glUnmapBufferOES`, `glProgramBinary` beside `glProgramBinaryOES`),
  sharing the code; 2.0's GLES2 extensions add their suffixed names
  (`glBindVertexArrayOES`, `glTexImage3DOES`, ...: 18 entries,
  `glFramebufferTexture3DOES` among them, which ES 3.0 lacks). Each ES3-only
  entry checks the context version first. 1.3's extensions gain what their
  specifications add with ES 3.0 or with the new extensions:
  `OES_draw_elements_base_vertex` adds `glDrawElementsInstancedBaseVertexOES`
  with any instancing (so in 2.0's GLES2 contexts too, with
  `EXT_instanced_arrays`) and `glDrawRangeElementsBaseVertexOES` with ES 3.0
  only.
- **Headers.** `<GLES3/gl3.h>` and `<GLES3/gl31.h>` (MIT),
  `<GLES3/gl3platform.h>` (Apache-2.0, as `gl2platform.h`) and
  `<GLES3/gl3ext.h>` (Khronos' empty placeholder, SGI Free Software License
  B 2.0; it points programs to `<GLES2/gl2ext.h>`) join `include/` (and so
  the SDK: `build_release.sh` copies `include/`); each carries its licence
  in the file. `<proto/ogles2.h>` must work with `<GLES2/gl2.h>` or the
  GLES3 headers included before or after it (the macros against Khronos'
  prototypes: `GL_GLES_PROTOTYPES`; the typedefs both declare), with GCC
  6.5, GCC 16 and vbcc: a probe compiled every way in stage A.
- **Library version 2** for the first release from this work (§1);
  `lib_Version` 2. The ABI version does not change: 1.x programs run on
  2.x. As between 1.x releases, a program linked with a newer
  `libogles2.a` needs at least that library (the dispatch-size check).
- **The compiler** keeps its one Mesa context: it is ES 3.1 already (§2).
  The library decides which GLSL versions a context accepts. What ES3 adds
  to the compiler:
  - **link state** that is not in the sources: transform feedback varyings
    and their buffer mode (ES 3.0 allows them with GLSL ES 1.00 shaders too),
    and `GL_PROGRAM_SEPARABLE` (stage D). It is sent with the link and
    applied before Mesa links (`_mesa_TransformFeedbackVaryings`,
    `_mesa_ProgramParameteri`). The program's hash covers it after the
    shaders and the bindings, written as the bindings' "BIND" pseudo-stage
    is (`FORMAT.md`): stage 0x58464256 ("XFBV") with the text: the buffer
    mode as four lowercase hex digits (`8c8c` interleaved, `8c8d`
    separate) and `\n`, then each varying name and `\n` **in the order
    given** (the order is the buffers' layout; "BIND" is sorted by name,
    this is not); and stage 0x53455041 ("SEPA") with the text `1` for a
    separable program; neither appears without its state. So two
    programs of the same sources with different link state never share a
    file;
  - **the files of ES3 programs** (a shader of `#version 300 es` or `310 es`,
    or ES3 link state) in format 2 (below), with what Appendix B.1 lists;
  - **variants by key** (stage A item 5; compute ones in stage D);
  - **the GLSL limits** of ES 3.0 and 3.1: every `gl_Max*` built-in and every
    limit Mesa's compiler and linker check comes from the context's
    `ctx->Const` (Appendix B.1); the library reports the same values, and
    where its own limit is lower `v3dglslc` lowers `ctx->Const` to it, as it
    does now for draw buffers and texture units. The GLSL ES 1.00 built-ins
    have the same value in both context versions (§2: 48 combined units from
    2.0), so one `ctx->Const` serves every program and a 1.00 program's file
    does not depend on the context that linked it.
  - **Request codes:** new ones (`V3DGLSL_LINK_ES3`, which names the stages
    it sends: vertex, fragment, compute, so one code serves whole,
    separable and compute programs; `V3DGLSL_COMPILE_VARIANT`; ...), as
    `V3DGLSL_LINK_BOUND` did: a field added to
    `struct V3DGLSLRequest` is read only for the codes that send it, so an older
    library's shorter request is never read past its end. The compiler keeps
    every old code (a 1.x library may use a 2.x compiler).
  - **An older compiler** answers a new code with result 3 (bad request), and
    links an ES3 program sent with an old code (by a 1.x library) into a
    format 1.0 file without its ES3 parts. A 2.x library therefore takes
    only a format 2 file for an ES3 program, whoever made it (1.3 made
    1.0 files of such programs, §1), and otherwise reports "the installed
    compiler is too old for OpenGL ES 3" in the program's info log; it
    serves GLES2 programs as 1.3 does (stage A item 5).
  - **A refused file no longer ends the lookup** (`program.c:926`): the
    library tries the next place (`PROGDIR:shaders/`, `PROGDIR:`, the
    cache), and if none has a file it can use, compiles the program into
    the cache, replacing a refused file there.
  - **Which compiler:** the library uses `LIBS:v3dglslc` first and a running
    server whatever its file (`compiler.c:28`, `:237`), so a program's own
    `PROGDIR:libs/v3dglslc` is shadowed by an older installed one
    (`ogles2.doc` says so). From 2.0 `v3dglslc` carries a `$VER:` string;
    `Install-ogles2` (which asks about the library and the compiler
    separately, `:20-53`) compares it with the installed one (the
    `Version` command) and replaces an older `LIBS:v3dglslc` whenever it
    installs the library (done 2026-10-23, §4 item 2).
  - **Caches across compiler changes.** A cached file is used whatever
    compiler made it (main plan, M7): the program's hash covers its sources,
    bindings and link state, nothing of the compiler, and the header's Mesa
    commit is the same for 1.x and 2.x compilers. So the installer of a
    release whose compiler changes existing programs' files clears
    `OGLES2Cache:` (it is only a cache), and test packages clear theirs as
    now. 2.0 is such a release (48 combined units change
    `gl_MaxCombinedTextureImageUnits`; a 1.3-cached file of a shader reading
    it keeps 32, which dEQP-GLES2's `builtin_variable` cases would catch).
    Shipped binaries (`PROGDIR:shaders/`) keep working; only a shader reading
    that built-in sees the old value.
  - The offline compiler writes the same files as the on-target one. Every
    compiler change rebuilds both compilers (host and m68k, byte-identical:
    `tools/v3dglslc/test/compare_v3dp.sh`).
- **The Mesa tree.** Stage A needs a Mesa patch of its own (item 5), which
  must not reach the 1.x compiler, so this work has a separate tree
  (2026-10-22), `~/mesa-build-gles3/mesa`, installed into
  `~/mesa-install-gles3`: the same Mesa commit with the same changes,
  configured afresh (a copy would not do: `build.ninja` holds absolute paths
  into the tree it was made in). The scripts take the tree from `MESA` and
  the prefix from `MESA_INSTALL`, which `local.env` sets; unset, they mean
  `~/mesa-build/mesa` and `~/mesa-install` as before: `build_mesa.sh`
  (which checks out a tree already there instead of cloning it),
  `apply_mesa_patches.sh`, `build_host.sh`, `replay.py` (and with it
  `build_m68k.sh`), `test/compare_v3d.sh`, the Makefile's host checks and
  `build_release.sh`. The compiler's Mesa changes, patches 0001-0014 of
  `tools/v3dglslc/patches/`, are applied by
  `dev/host-setup/apply_mesa_patches.sh` (added 2026-10-21: before it no
  script applied them, and `~/mesa-build/mesa` held them as uncommitted
  edits; on a clean checkout of the pinned commit it gives that tree's
  changes exactly), on WSL1 with `patch_drmshim.sh`'s `memfd_create`
  fallback as well (that script still names `~/mesa-build/mesa`); stage A's
  own patch is 0015 (2026-10-22, `v3d_compile_variant`, item 5), in this
  work's tree only (§1, "Working on it": the trap). A new machine: `. ./local.env`, `build_mesa.sh`,
  `apply_mesa_patches.sh`, `ninja -C $MESA/build install`.
- **`.v3dp` format 2 for ES 3.x programs.**
  - **Why a major version:** a 1.x reader skips unknown sections and accepts
    any minor version, so an ES3 program's file read by 1.x would draw
    wrongly instead of failing (an ES3 program on 1.x gets a GLES2 context
    and looks for its binaries as usual). ES3 programs' files therefore
    carry major version 2, which 1.x refuses at the header ("unknown format
    major version"). GLES2 programs' files stay 1.0, readable by 1.x; the
    reader's new recipe types (Mesa's 0-46) are not a format change. A GLES2
    context refuses a format 2 file from `glProgramBinary`.
  - **Provisional until 2.1:** release 2.0 writes no format 2 file (it has no
    ES3 contexts, and GLES2 programs have no ES3 link state), so the format is
    frozen at 2.1, the first release that writes it. Until then the `gles3`
    builds write major version 0x8002, which released readers refuse; a merge
    that renumbers sections or stage values only invalidates such test files.
  - **Must-understand features:** format 2 files carry, in the `PROGRAM`
    section, a word of feature bits a reader must know (transform feedback,
    uniform and storage blocks, compute, separable programs, explicit
    locations, ...); a reader refuses a file with a bit it does not know. So a
    2.1 library refuses a file an ES 3.1 program got from 2.2 (from
    `glProgramBinary`, or a cache after a downgrade) instead of misreading it,
    and later additions that older readers may skip go in new sections and
    minor versions, as in 1.x.
  - **Contents** (Appendix B.1 gives where each comes from in Mesa): uniform
    blocks and storage blocks with their members and initial bindings, and for
    each stage the map from its block numbers (`QUNIFORM_UBO_ADDR`'s unit
    minus 1) to the program's blocks; each stage's storage block count (an
    atomic counter buffer is the stage's storage slot `num_ssbos + binding`);
    atomic counter buffers; explicit uniform locations and `layout(binding)`
    values (stage D); fragment outputs and their locations; transform feedback
    varyings, buffer strides and Mesa's transform feedback specs (stage C);
    every program's interface: the `GL_PROGRAM_INPUT` and `GL_PROGRAM_OUTPUT`
    entries of Mesa's `ProgramResourceList`, built-ins included (dEQP-GLES31
    expects `gl_VertexID`, `gl_FragCoord`, `gl_Position`, `gl_FragDepth`,
    `gl_NumWorkGroups`, `es31fProgramInterfaceDefinitionUtil.cpp:1400-1426`),
    with name, type, array size, precision, location, interpolation and the
    stages that reference them, written from stage A so that 2.1's files serve
    2.2's `glGetProgramResource*` and pipeline matching; compute programs
    (stage value 5, Mesa's `MESA_SHADER_COMPUTE`, with local size and shared
    memory size; named apart from `OGLES2_VARIANT_CS`, which is the coordinate
    shader, 1); and in `ATTRIBUTES` integer attribute types (which 1.x would
    read as float) and which inputs have a location of their own
    (`layout(location)`: bit 30 of the location word, from Mesa's
    `explicit_location`, `shader_types.h:1191`). In 1.0 the location is
    informational (`FORMAT.md:145`) and the library numbers inputs itself,
    bindings first (`program.c:1060`); an explicit location is kept, and
    `glBindAttribLocation` cannot move it. Stage values 2 and 3 are reserved
    for geometry shaders' binning and render variants (§10). Programs of one
    stage are allowed (separable programs, compute).
  - **More sections:** the 2.x reader takes up to 256 sections, allocated
    rather than on the stack (1.x: 64); a `.v3dx` file (stage A) that would
    exceed that drops its oldest variants.
  - `FORMAT.md` records format 2 (major version 2, minor 0) before stage A
    writes its first file.
- **Tests:**
  - each stage brings its ogles2 tests and its dEQP groups;
  - the dEQP port (`ports/deqp`, Appendix B.5) creates ES3 contexts with the
    tag (stage A) and builds `deqp-gles3` (stage B) and `deqp-gles31` (stage
    D), their must-pass lists split by group as with GLES2. The lists that
    hold on any platform are `main` (a 256x256 RGBA8 default framebuffer
    with depth and stencil) and `multisample` (the same with 4 samples,
    `rgba8888d24s8ms4`): `gles3-multisample.txt` (4,635 cases) and
    `gles31-multisample.txt` (239) beside `gles3-main.txt` and
    `gles31-main.txt` (`aosp_mustpass/main/mustpass.xml`; the rotation and
    565 lists are Android's);
  - **the lists are long:** dEQP-GLES3's main must-pass list has 44,768
    cases (21,239 in
    `shaders`), 2.6 times GLES2's: at GLES2's rate about 6.5 hours, more if
    its shaders compile more slowly on the Amiga. dEQP-GLES31's has 37,802, of
    which about 16,000 test ES 3.2's extensions (`copy_image` alone 12,395;
    `tessellation`, `geometry_shading`, `texture.border_clamp`,
    `texture.texture_buffer`, 1,121 of `debug`'s 1,617 ...) and report "not
    supported" here, quickly; `debug`'s other 496
    (`negative_coverage.get_error`) test ES 3.1's errors without
    `KHR_debug` (`es31fDebugTests.cpp:850-861`) and run (stage D item 6);
  - **`amictld`'s large-reply stall** (main plan §13, postponed) is fixed
    before stage B's first GLES3 runs;
  - **nothing of ogles2 in the test A1200's `LIBS:`** while packages run (the
    station's rule, `CLAUDE.md`: check `List LIBS:ogles2#? LIBS:v3dglslc`
    before tests, since a package's `libs` are added after the system's, §2),
    so the regression packages run unchanged; packages of this work also put
    their `libs` first (`Assign LIBS: <package>:libs SYS:Libs`, the old
    assignment restored afterwards; never a bare `Assign LIBS:`, which
    removes the assign, `CONTRIBUTING.md`) and flush the library first
    (`Avail FLUSH`);
  - the GLES2 regression set runs unchanged at each stage's end, as at 1.3's
    release: dEQP-GLES2 (`m7-1`, 17,165 cases), `m5-1`, the corpus `m2-5`,
    `m6-6`, `m6-8`, `m6-10`, SDL `m6-3`/`m6-4`, glmark2 `m6-7`, `m7-2`,
    `m7-3`, `m7-4`, Quake III `m6-5`, the examples, and gl4es `m6-1` and
    Quake III through gl4es `m6-2` (gl4es compiles its shaders at run time,
    so stage A's variant path meets it first, and it looks for three of
    2.0's extensions);
  - **speed:** Quake III's timedemo (80.4 fps at 1.3) and glmark2's score
    (242) are measured at each stage's end against 1.x on the same day; a
    GLES2 program may get at most 2% slower from ES3 state checks (the 68k
    CPU is the limit).
- **Docs:** `ogles2.doc` gains the new extensions (`--background--`
  EXTENSIONS and LIMITS), an ES3 section with its deviations
  (`--deviations--`), the tag (`aglCreateContext2`); `GettingStarted.txt` an
  ES3 chapter, `docs/SDL.md` ES3 contexts from SDL (2.1), the SDK the ES3
  headers, the main plan's Appendix B.3 the new extensions.

## 4. Stage A: GLSL ES 3.00 and draw-time variants (3-4 weeks)

The base everything else stands on.

1. **ES3 contexts in the library** (behind the tag, test builds only until
   2.1, §3): a version field in `struct OGLES2Context`, the tag in
   `known_tag`, version strings, the limits ES 3.0 raises (`GL_MAX_*`, §2)
   and its new queries (`GL_MAJOR_VERSION`, `GL_MINOR_VERSION`,
   `GL_NUM_EXTENSIONS`, `GL_MAX_ELEMENT_INDEX` 2^24 - 1,
   `GL_MAX_ELEMENTS_VERTICES` and `_INDICES`, `GL_MAX_SERVER_WAIT_TIMEOUT`,
   `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT` 256, ...; all of ES 3.0's state
   tables in the end, which dEQP-GLES3's `state_query` checks), queries of
   more than 8 values and of 64-bit ones (`state.c`), `glGetStringi`,
   `glGetInteger64v`. 48 combined texture units in every context (§2). The
   extensions per context version: one table of names with a version mask
   builds both `GL_EXTENSIONS` and the `glGetStringi` list (today one
   string, `context.c:311-322`; dEQP-GLES3 compares the two,
   `es3fImplementationLimitTests.cpp:463-494`). ES3 contexts list every
   extension GLES2 contexts have (1.3's 33, then 2.0's), as Mesa does, and
   the ES3-only ones as they land (`EXT_color_buffer_float`, stage B item
   10); each must work there, which settles some of dEQP-GLES3's choices:
   with `EXT_color_buffer_half_float` listed, RGB16F renderbuffers and
   textures are colour-renderable (stored as RGBA16F;
   `es3fNegativeBufferApiTests.cpp:1116-1121`, `es3fFboTestCase.cpp:230-236`),
   and (dEQP-GLES3 draws `EXT_draw_buffers`'
   GLSL ES 1.00 shaders into two targets,
   `es3fShaderFragDataTests.cpp:242-250`; `ARB_texture_rectangle`'s sampler
   compiles in `#version 300 es` shaders with its `#extension`, tested
   2026-10-19). The share groups' version check (§3). The headers' probe
   (§3).
   **Done (2026-10-22; on the A1200 the same day, run `ogles2-m8-1`):**
   `OGLES2_CCT_PISTORM_ES_VERSION` (`OGLES2_CCT_PISTORM_MIN + 6`), 30 only
   in `make ES3=1` builds (`OGLES2_ES_MAX`), 31 refused until stage D;
   `ctx->es_version`; ES3-only entries refuse in GLES2 contexts through
   `ogles2_es3_entry`; the version strings from `OGLES2_LIB_VERSION` and
   `_REVISION`; one extension table with a version mask (`context.c`)
   behind `GL_EXTENSIONS`, `GL_NUM_EXTENSIONS` and `glGetStringi`;
   `glGetInteger64v`; queries of up to 16 values; ES 3.0's limits in ES3
   contexts (`state.c`'s `es3_limits`); 48 combined texture units in every
   context (`OGLES2_MAX_TEXTURE_UNITS`, and `v3dglslc` sets the same); the
   share groups' version check; `include/GLES3/` (`gl3.h`, `gl31.h`,
   `gl3platform.h` from the registry as of 2026-08-03, the same as our
   `gl2.h`). Found on the way: Khronos no longer publishes `gl3ext.h` (its
   URL answers 404; ES3 extensions are in `gl2ext.h`), so the SDK's is a
   placeholder of ours that includes `<GLES2/gl2ext.h>`; and the probe
   (`make es3-header-probe`, five orders, GCC 6.5, GCC 16 and vbcc) found
   that vbcc refuses a program that includes `<GLES2/gl2.h>` before
   `<proto/ogles2.h>` and `<GLES3/gl3.h>` after it (Khronos' prototypes
   come back under the wrappers' names), so `<proto/ogles2.h>` now ends by
   setting `GL_GLES_PROTOTYPES` to 0; vbcc's warnings about typedefs
   repeated inside Khronos' own headers (`gl2ext.h`'s
   `GL_MESA_sampler_objects`) are accepted. **The limits:** `v3dglslc -L`
   prints every ES 3.0 and 3.1 limit as its Mesa context answers it, and
   `m8_es3 LIMITS` the library's in the same format; the two lists are
   identical (42 lines up to ES 3.0). `v3dglslc` sets the limits that are
   the library's own besides those of §3 (`GL_MAX_ELEMENT_INDEX` 2^24 - 1,
   `GL_MAX_SERVER_WAIT_TIMEOUT` 0, `GL_MAX_TEXTURE_LOD_BIAS` 15, where Mesa
   now offers 16), so the lists stay comparable; Mesa's others stand (15
   uniform blocks a stage, 108 uniform buffer bindings, 69 combined
   blocks). `m8_es3` (12 checks with a release build, 23 with an ES3
   build) passes under vamos and on the A1200 (`tests/a1200/results/ogles2-m8-1-20261022-210505`: 14 of 14 with the
   release build, 28 of 28 with the ES3 build, the limits read there
   identical to `v3dglslc -L`, the smoke test as before); the library was
   243,024 bytes then (+2,712).
   "All of ES 3.0's state tables" grow with the features that own the
   state (stages B and C).
2. **GLSL ES 3.00 programs through the compiler:** the version check per
   context (`glCompileShader`), the new request codes, ES3 link state and
   its hash (tested on the host, `tools/v3dglslc/test/`: the library sends
   transform feedback varyings from stage C, the separable flag from stage
   D), the format 2 writer (`v3dp/v3dp_writer.c`; Appendix B.1),
   key-state draws that bind every sampler type (also `sampler3D`, which
   2.0's `OES_texture_3D` brings to GLES2 programs) and draw into targets
   of the fragment outputs' types (Appendix B.1), the offline compiler's
   output for ES3 programs, 48 combined texture units, a `$VER:` string.
   The front end needs nothing: `in`/`out`, `layout(location)`, integer and
   unsigned types, non-square matrices, `texelFetch`, `textureSize`,
   `textureLod`, switch, bit operations, array constructors, `flat`,
   `centroid` and `invariant` already compile (§2).
   **Done so far (2026-10-22, on the host):** format 2 in `v3dp/FORMAT.md`
   (provisional, major 0x8002) and in the writer: `PROGRAM`'s feature word,
   `UNIFORMS`' block fields, `ATTRIBUTES`' explicit-location bit,
   `UNIFORM_BLOCKS` with each stage's map, `INTERFACE`; the `.v3dx` layout
   (`VARIANT_OF`) is specified for item 5. The writer refuses storage blocks,
   atomic counters and images until stage D. `v3dp_dump.py` decodes format
   2. The key-state draws of GLSL ES 3.x programs: a zeroed 64 KB buffer at
   every binding the uniform blocks name (Mesa asserted in `u_upload_alloc`
   without one), integer inputs through `glVertexAttribIPointer`, every
   column of non-square matrices, and one target whose colour attachments
   match the outputs' locations and types (RGBA8, RGBA8I, RGBA8UI) instead
   of 1.3's two- to four-target RGBA8 draws, which gave a `uvec4` output
   nine variants no draw uses. Seen in the test program (`es3.vert`,
   `es3.frag`): Mesa gives the vertex stage the fragment-only block too
   (its map has all three), so the map is what the state tracker binds, not
   what each shader declares. GLES2 programs' files are byte for byte what
   the compiler wrote before these changes (8 of 8). Every sampler type in
   the key-state draws: the three of GLSL ES 1.00 keep their textures, the
   others (3D, 2D array, the three shadow ones, integer and unsigned) get
   1x1 textures of their kind, made when first bound (`more_samplers`):
   RGBA8, DEPTH_COMPONENT16 compared, RGBA8I, RGBA8UI, and for the 32-bit
   variants RGBA32F, DEPTH_COMPONENT32F, RGBA32I, RGBA32UI. Seen in the
   test program (`es3tex.vert`, `es3tex.frag`): depth textures return 32
   bits even at 16 bits of depth, so shadow samplers have their
   `sampler_is_32b` bit in the plain variants too; 8-bit integer ones
   return 16. The offline compiler writes format 2 for ES3 programs as it
   is (`v3dglslc -o`), and `compare_v3dp.sh` checks the m68k compiler's
   ES3 files against the host's. The version check per context
   (`glCompileShader`'s `glsl_version`, the library's own reading of the
   `#version` line after whitespace and comments, before any record or
   compiler: `300 es` refused in GLES2 contexts, `310 es` until stage D,
   with the reason in the info log; `m8_es3` checks both). **The request
   codes wait for stage C (decided 2026-10-22):** the writer chooses the
   format itself (a GLSL ES 3.x program gets format 2 whatever request
   asked for it), so a 2.x compiler answers `V3DGLSL_LINK` with a format 2
   file, and a 1.x compiler's 1.0 file of an ES3 program is refused by the
   library ("a GLSL ES 3.x program in a format 1.0 file (made by an older
   compiler)"), which is the "compiler too old" answer this item asked
   for. `V3DGLSL_LINK_ES3` comes when there is ES3 link state to send
   (stage C's transform feedback varyings, stage D's separable flag), and
   `V3DGLSL_COMPILE_VARIANT` with item 5. **The `$VER:` string done
   (2026-10-23, on the A1200):** `v3dglslc` carries `$VER: v3dglslc
   <version>.<revision> (<date>) PiStorm V3D 4.2`, the library's
   (`OGLES2_LIB_VERSION`, `_REVISION`, `_DATE` in `ogles2_internal.h`, which
   `build_m68k.sh` and `build_host.sh` read and pass as `V3DGLSLC_VER`),
   in the data hunk beside the stamp (still in the file's last 64 KB, which
   `ogles2_compiler_id` reads). `Install-ogles2`, after installing the
   library, replaces a `LIBS:v3dglslc` older than it or without a version
   string (`Version ... 1 3 FILE` answers 5 and 20) without asking, and
   then skips the compiler question; every copy is checked with `Version`
   afterwards, because AmigaDOS' `Copy` answers 0 when it copied nothing
   (found on the A1200: "object is in use", "protected from deletion").
   `build_release.sh` checks that the shipped compiler's `$VER:` and every
   `Version` line of the installer carry the library's numbers. Tested on
   the A1200 against a scratch drawer standing for `LIBS:`: a 1.x compiler
   replaced, the same version kept, none installed by the compiler step, a
   protected one refused with rc 20. Item 2 is done.
3. **The reader and the draw:** format 2 (§3) and the lookup that goes on past
   a refused file; the recipe types GLSL ES 3.00 brings (Mesa's
   `QUNIFORM_TEXTURE_WIDTH`, `_HEIGHT`, `_DEPTH` and `_ARRAY_SIZE` for
   `textureSize`: the base level's size, which the shader minifies for the lod
   itself, and the layer count, `drv/v3d_uniforms.c:58-80`;
   `QUNIFORM_GET_UBO_SIZE`, whose data is the stage's plain block index *i*,
   where `QUNIFORM_UBO_ADDR`'s unit is *i* + 1, `:341-349`; ...; 1.3
   implements 0-7, 16, 26, 27, 31, 32, 42-46); unsigned uniforms
   (`glUniform*ui`, `glGetUniformuiv`) and non-square matrix uniforms
   (`glUniformMatrix2x3fv`, ...: `type_shape` in `program.c` learns them),
   with `transpose` allowed in ES3 contexts; non-square matrix attributes
   (`mat2x3` ... `mat4x3`: their columns, as the writer already stores every
   matrix's column mask, `v3dp_writer.c:473-489`); every sampler type of GLSL
   ES 3.00 as a uniform (`type_shape` and `ogles2_sampler_kind`,
   `ogles2_internal.h:654`, know 2D, cube and rectangle samplers only, and
   take the rest for 2D): until stage B item 8 brings their textures, 3D,
   array, shadow and integer samplers read the incomplete texture's (0, 0, 0,
   1); the draw's side of integer samplers (the TMU's integer return; 32 bits
   for 32-bit formats, `sampler_is_32b`) and of uniform blocks
   (`QUNIFORM_UBO_ADDR` for blocks other than the default one, through the
   stage's block map; a block with no buffer reads a zeroed block of the
   library's until stage B item 6 binds buffers); explicit locations of vertex
   inputs (§3) and fragment outputs (`glGetFragDataLocation`).
   **Done (2026-10-22; on the A1200 the same day):** the reader takes
   majors 2 and 0x8002, refuses unknown feature bits, takes up to 256
   sections (an allocated index), and reads `UNIFORMS`' block fields,
   `ATTRIBUTES`' explicit locations, `UNIFORM_BLOCKS` (into
   `OGLES2Image`'s `ublocks` and each stage's map) and `INTERFACE`; a
   variant may read `QUNIFORM_UBO_ADDR` units its stage's map names; the
   recipe types 10-13 and 21. The lookup goes on past a refused file, and
   the compiler then replaces the cache's; an ES3 program takes only a
   format 2 file; `glProgramBinaryOES` refuses format 2 in a GLES2 context.
   `glUniform*ui`, `glGetUniformuiv`, the six non-square
   `glUniformMatrix*fv`, `transpose` in ES3 contexts (GLES2 contexts still
   refuse it), every GLSL ES 3.00 sampler type as a sampler uniform; the
   new ones read the fallback texture (`OGLES2_TEX_NONE`) until stage B.
   Uniform blocks read a zeroed 64 KB block of the context's
   (`ubo_copies`); `textureSize` from the base level (Mesa's
   `get_texture_size`). Explicit attribute locations come before
   `glBindAttribLocation`'s. 18 more dispatch entries (182). **Trap:** the
   built-in image's structures are initialised by position
   (`program.c`), so new fields of `OGLES2Variant`, `OGLES2Image`,
   `OGLES2ImageAttrib` and `OGLES2ImageUniform` go at their ends, with
   explicit values there (an attribute's `location` 0xff, a uniform's
   `block` -1). Tests: `m8_es3` links the ES3 test programs from their
   files and checks the uniforms (42 checks); the new `m8_draw` draws
   GLSL ES 3.00 programs into a bitmap (`tests/a1200/results/
   ogles2-m8-1-20261022-212632`: 5 of 5, the flat varying, a block read as
   zeros, `textureSize` at two base levels); the GLES2 drawing tests as at
   1.3 (`ogles2-m7-4-20261022-212706`: 23, 44, 33 and 29 of the same).
   The library is 251,840 bytes (+11.5 KB on 1.3.1). Left for stage B:
   the integer samplers' textures.
4. **`flat` and `centroid` varyings, `gl_VertexID`:** the `FLAT_SHADE_FLAGS`
   and `CENTROID_FLAGS` packets from the fragment variant's flags, as Mesa's
   `emit_varying_flags` does (Appendix B.3), instead of the "zero all"
   packets alone. 1.3 already sets the shader record's vertex and instance
   ID flags, but its draws number the vertices from 0 (§2). For a vertex
   shader that reads `gl_VertexID` (prog data's `uses_vid`), a draw passes
   its real first vertex (`VERTEX_ARRAY_PRIMS`'s first index) and its
   indices unrebased, with each attribute's address moved back by that
   first vertex, or the lowest index, times its stride, so the copies stay
   as small as now; where that address would leave GPU-reachable memory
   (wrap past 0, or fall below what `gpu_reachable` accepts) the copy
   starts at element 0 instead. The hardware gives the shader the vertex's
   index from the VPM (`nir_to_vir.c:2409-2421`, `:3719-3721`), and Mesa
   draws with the real first vertex and the indices as given and passes
   conformance, so that index includes them. `gl_InstanceID` is 0 until
   stage B's instanced draws.
   **The flags done (2026-10-22, on the A1200):** `emit_state` sends the
   fragment shader's flat, no-perspective and centroid flags when the
   fragment variant changes (`varying_flags`, Mesa's
   `emit_varying_flags`), and a "zero all" packet only where an earlier
   shader of the pass set a kind, so GLES2 programs send nothing new
   (`m8_draw`'s flat triangle). **`gl_VertexID` done (2026-10-22, on the
   A1200):** a draw whose binning or vertex shader reads it numbers from
   `first` (`VERTEX_ARRAY_PRIMS`' first index; the indices as given plus
   the base vertex, written as `index - min + first`), each array's
   address moved back by `first` elements, a copy from element 0 where
   that address would leave the memory the GPU reaches; indices that
   outgrow 8 or 16 bits so are written as 32-bit ones. Other draws are as
   before. `m8_draw` (`tests/a1200/results/ogles2-m8-1-20261022-222729`, 9
   of 9): `glDrawArrays` from vertex 3, indices as given, a base vertex, and
   8-bit indices with base vertex 297; the covering triangle lies only at
   the vertices each draw names, so a wrong address would draw nothing. The
   GLES2 drawing tests as before (`ogles2-m7-4-20261022-222739`).
5. **Draw-time variants by key.** The two special files (`.v3dw`, `.v3da`)
   become one general path, which ES3's states need (integer and float
   render targets, sRGB, multisampled framebuffer objects, transform
   feedback, integer samplers):
   - the reader keeps each variant's whole key;
   - the library computes per draw the key fields these states change, as
     Mesa's `v3d_update_compiled_fs` and `_vs` do (Appendix B.1 lists each
     field's source): `software_blend` (set when any bound colour target
     cannot be blended in the tile buffer, internal types other than 8 and
     16F, **even with blending off**), with it `color_fmt[i]` (format and
     swizzle) and `blend[i]` for every target; `f32_color_rb`, `norm_16`,
     `snorm`; `sampler_is_32b`; `msaa`, `sample_alpha_to_coverage`,
     `sample_alpha_to_one` (always 0: GLES has no alpha-to-one),
     `ignore_sample_mask`; `can_earlyz_with_discard`, always 0 (what
     `v3dglslc`'s default draws give, with the depth test and depth writes on,
     `v3dglslc.c:166`; Mesa sets it only without depth or stencil writes and
     without an active occlusion query, `v3d_program.c:687-691`, and a variant
     with it set would count wrong samples during a query); the vertex
     shader's used outputs (from the fragment variant chosen; for the binning
     variant, the transform feedback varyings from the file, none without
     them); and `logicop_func`, always `PIPE_LOGICOP_COPY` (12; GLES has no
     logic op, and 0 would be CLEAR, which makes the shader write zeros). It
     matches them (`sampler_is_32b` matches when the variant's bits include
     the draw's: a 32-bit return reads 8-bit textures correctly too, which is
     why 1.3's `.v3dw` variants serve any mix); `msaa` and
     `ignore_sample_mask` only when the draw has alpha to coverage or
     `software_blend` (below ES 3.2 they change the code only there: the
     shader's per-sample tile-buffer reads, `nir_to_vir.c:2713-2716`, and
     alpha to coverage; the sample-mask output and interpolation at a sample
     are ES 3.2's), so the variants 1.3 compiled without MSAA (`msaa` 0,
     `ignore_sample_mask` 1) keep serving a multisampled default
     framebuffer, as now (`draw.c:1483`); the other fields keep 1.x's
     matching, so **a GLES2 draw that finds its variant in 1.3 finds the same
     one** (checked in stage A: Quake III from a fresh install compiles
     nothing, with 4x MSAA and without, and the release examples draw from
     `PROGDIR:shaders/` alone).
     The computed keys are checked against the keys `v3dglslc` stores for the
     same states (a host test);
   - on a miss it sends the program's sources (and bindings, and any ES3
     link state), the stage and the key to the compiler
     (`V3DGLSL_COMPILE_VARIANT`). The compiler links the program, draws once
     to get the driver's uncompiled shaders, and compiles that key directly
     through a Mesa patch: an exported wrapper of `v3d_get_compiled_shader`,
     which is `static` now (Appendix B.1). The variant goes to the program's
     **variant file in the cache**, `<hash>.v3dx`, which the compiler
     rewrites with each new variant: it reads the cache's file, adds the
     variant and writes a temporary file it then renames (the writer keeps
     at most 64 variants now, `v3dp_writer.c:132`; a `.v3dx` holds up to 256
     sections). Its header counts sections and holds the CRC; its format
     version is the program's file's (0x8002 for an ES3 program until 2.1,
     §3). A variant's uniform recipes index the program's constant buffer
     as its link laid it out (`FORMAT.md:171-172`), so a `.v3dx` records
     the CRC of the program sections it was compiled with (`STAGE_PARAMS`,
     `UNIFORMS`, `ATTRIBUTES`): the library takes variants only from a
     `.v3dx` whose CRC is that of the `.v3dp` it loaded, sends that CRC
     with the request, and the compiler, which links the same sources,
     answers a mismatch (another compiler made the `.v3dp`) with no variant,
     so the draw goes on as without a compiler (below). Not into the
     program's `.v3dp`: that one may be shipped in `PROGDIR:shaders/`,
     read-only, and is found before the cache. The library reads
     `<hash>.v3dx` from the three places it looks for a `.v3dw` now
     (`PROGDIR:shaders/`, `PROGDIR:`, the cache), all of them: the first two
     hold a program's shipped variants, the cache the ones compiled here;
   - the library keeps a linked program's sources, bindings and ES3 link
     state for this (1.3 keeps the sources, `program.c:1172-1173`); a
     program restored by `glProgramBinary` has none of them, so its missing
     variants follow the rule below;
   - the draw waits for the compile once (about 0.1 s, as a `.v3da`; longer
     for a large program), later starts load it;
   - **without a compiler, or with one too old, or without the sources,** the
     draw uses the closest variant and logs it once per program, as 1.x
     does (main plan §7.8), except where the closest variant cannot be right
     (a render target of another kind: integer against float; transform
     feedback outputs missing): that draw is skipped and logged
     (`draw: no variant for program <hash> and no compiler for it`).
     Programs that need such states ship their `.v3dx`;
   - **Done so far (2026-10-22, on the host), in four steps of which two
     are in:** (1) the reader keeps every variant's whole key as the file
     has it (`OGLES2Variant`'s `key`: 88 bytes vertex and binning, 296
     fragment), so the library can compute a draw's key in the same layout
     and compare bytes; (2) Mesa patch 0015 (`v3d_compile_variant`, at the
     end of `v3d_program.c`: the shader the last draw bound, compiled for
     exactly the key given), `v3dp_key_parse` (the inverse of the writer's
     key fields), the `.v3dx` writer (`v3dp_write_variants`: the existing
     file's variants of the same program and layout kept, new ones added,
     the oldest dropped past 253, written to a temporary file and renamed),
     and `v3dglslc -x KEYS` to compile given keys from the command line.
     `tools/v3dglslc/test/variant_roundtrip.py`: every variant of every
     test program, compiled again from its key alone, comes out byte for
     byte (`mrt`'s 14 included), so the file's key fields are all of Mesa's
     key; the `.v3dx`'s `VARIANT_OF` is the `.v3dp`'s layout CRC, and a
     second run adds nothing. (3) and (4), for OpenGL ES 3 programs
     (format 2 images), done 2026-10-22 on the A1200: the draw builds the
     key it needs from the closest variant's (1.x's choice) with the fields
     stage A's states give (primitive class, `cbufs` from the draw buffers,
     no software blending, every target's format and blend 0,
     `f32_color_rb`, `norm_16`, `snorm` 0, the logic op COPY; a .v3dw's or
     .v3da's 32-bit samplers and alpha to coverage as their image has
     them), takes a fragment variant of that key from the program's file or
     its `.v3dx` images (shipped in `PROGDIR:shaders/` or `PROGDIR:`, the
     cache's), else has it compiled (`V3DGLSL_COMPILE_VARIANT`: sources,
     bindings, the keys, the program file's layout CRC, which the compiler
     checks), then the vertex variant whose used outputs are that fragment
     variant's input slots the same way (`es3_variants`, `draw.c`). A
     `.v3dx` is taken only when its hash, `VARIANT_OF` and uniform blocks
     are the program's (`try_x`); a newer cache `.v3dx` keeps the older
     image chained (programs may hold its variants). Without a compiler,
     or with one too old, the closest variant is used and logged once.
     `m8_draw`'s `es3out` (two outputs, one `uvec4`, precompiled for a
     float and an unsigned target) drawn into RGBA8: with the compiler its
     fragment variant is compiled at the first draw (180 ms on the A1200),
     without it read from the cache's `.v3dx`
     (`tests/a1200/results/ogles2-m8-1-20261022-232345`, 10 of 10 both
     ways); the GLES2 drawing tests as before
     (`ogles2-m7-4-20261022-232417`). **Decided:** GLES2 programs keep 1.x's
     choice in stage A (so the same variants as in 1.3, trivially): their
     key fields change with no state stage A brings; they join the key path
     when stage B's float, integer and sRGB targets reach GLES2 contexts.
     **The rest done (2026-10-23, on the A1200):** an ES3 program's keys
     carry the draw's 32-bit samplers (Mesa's `sampler_is_32b`, a bit per
     texture index whose bound texture returns 32 bits, both stages; a
     variant matches when its bits include the draw's) and alpha to coverage
     (`msaa` and `sample_alpha_to_coverage` set, `ignore_sample_mask` clear,
     as Mesa's multisampled job), so it never asks for a `.v3dw` or `.v3da`
     (`V3DGLSL_LINK32`, `_A2C`); shipped or cached ones are still looked in
     (`ogles2_image_wide_file`, `_a2c_file`), GLES2 programs keep 1.x's
     path. A missing fragment variant comes with its vertex variant in one
     request: the vertex key's stage word carries
     `V3DGLSL_KEY_OUTPUTS_FROM_FS` (0x100, FORMAT.md), and the compiler
     takes its used outputs from the fragment variant it compiled just
     before (`v3dp_last_fs_inputs`, from the capture hook: no Mesa change);
     the program's last choice is kept by the sampler bits and alpha to
     coverage too (`var_bits`, `var_a2c`). The keys are computed and matched
     in `ogles2/variant_key.c`, which the host test
     `make variant-key-test` (`tools/variant_key_test`) checks against
     Mesa's own keys in the host compiler's `.v3dp`, `.v3dw` and `.v3da`
     (`v3dglslc -a`, new) of 15 test programs: every key computed from its
     own state is Mesa's byte for byte, the `.v3da`'s and `.v3dw`'s follow
     from the plain ones, the vertex keys from the fragment variants' inputs
     (486 checks; the keys of integer targets wait for stage B).
     `variant_roundtrip.py` checks the one-request path: every fragment
     variant with its vertex variant, the latter's outputs wiped, gives both
     byte for byte. `m8_draw` (17 checks; `tests/a1200/results/
     ogles2-m8-1-20261023-143820`): a float texture through the shipped
     `.v3dw`, alpha to coverage in a 4x MSAA context compiled by key (one
     request, 100 ms), both together, then all of it without the compiler
     from the cache and from `.v3dx` files shipped in `PROGDIR:`. Left for
     stage B: skipping a draw whose closest variant cannot be right
     (integer targets).
   - existing `.v3dw` and `.v3da` files stay readable. With a compiler that
     knows `V3DGLSL_COMPILE_VARIANT` the library asks for no new ones; with
     an older one it asks for them as 1.3 does (`V3DGLSL_LINK32`,
     `V3DGLSL_LINK_A2C`), so a 2.x library with a 1.x compiler serves GLES2
     programs exactly as 1.3 does. The compiler keeps both codes.
6. **The dEQP port** creates an ES3 context (Appendix B.5: the platform's
   API check, the tag's 30, `glw::initES30`).
   **Done (2026-10-23, on the A1200):** the platform takes ES 2.0 and 3.0,
   passes the tag's 20 or 30, refuses 3.0 as not supported when the library
   answers `OGLES2_CCE_UNSUPPORTED` (a release build) or gives a context
   whose `GL_VERSION` is not "OpenGL ES 3.0" (a library before 2.0 ignores
   the tag), and loads `glw::initES30`. `ports/deqp/build.sh` builds
   dEQP-GLES3 with `MODULE=gles3` into `$B/deqp-gles3` (its own copy of the
   sources; `modules/gles3` without `tgl45es3*`, `data/gles3`, the
   GraphicsFuzz cases; the patches of `modules/gles2` moved to
   `patches/gles2/`), `caselists.sh` splits `gles3-main.txt` with
   `MODULE=gles3`, `run_groups.py` takes `--package=m8-2`; package `m8-2`
   (Appendix B.5) is `m7-1`'s for dEQP-GLES3 with the ES3 sync build.
   **Found on the A1200:** the first run hit SmartCrash in its first case:
   `aglGetProcAddress` answers 0 for the 88 ES 3.0 functions the library
   lacks (the list the run's `.txt` prints: VAOs, samplers, queries, sync,
   transform feedback, 3D textures, `glDrawBuffers`, `glReadBuffer`,
   `glBlitFramebuffer`, `glClearBuffer*`, `glMapBufferRange`, the uniform
   block queries, `glVertexAttribI*`, instancing, ...), and `glu::resetState`
   calls many of them after every case. In ES 3.0 contexts the platform now
   gives each a stub that does nothing, returns 0 and names it once (a throw
   would end the run: an exception in a case's deinit terminates dEQP); ES
   2.0 contexts keep null pointers, which `patches/gles2/0004` tests for.
   Then `dEQP-GLES3.info.vendor` passes in an ES 3.0 context, freed cleanly
   (`tests/a1200/results/ogles2-m8-2-20261023-135140`), but the run ends
   there: `resetState` checks `glGetError` after each part of the state, and
   `glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX)` is `GL_INVALID_ENUM`, which
   terminates dEQP. So dEQP-GLES3 runs one case a run until stage B gives
   the state `resetState` touches (§5 item 13). dEQP-GLES2 as before
   (`ogles2-m7-1-20261023-135452`: info 6 of 6).
7. **Tests:** a new `m8_es3` in a new package `m8-1` (GLSL ES 3.00 features
   against expected pixels,
   `flat`, `centroid` and `gl_VertexID` among them, a GLES2 context
   refusing `#version 300 es`, a build without the option refusing the
   tag's 30), the variant path (a program drawn in states with no
   precompiled variant, then from the cache without the compiler, then from
   a shipped `.v3dx`), a 1.0 file of an ES3 program refused and replaced;
   gl4es `m6-1` and `m6-2` with the regression set.
   **Done but for gl4es (2026-10-23, on the A1200):** `m8-1` has seven
   steps: `smoke`, `m8_es3` with the release build (14), `m8_es3 ES3` (42),
   the limits (identical to `v3dglslc -L`), `m8_draw` with the compiler,
   without it, and with the `.v3dx` files moved beside the program (17
   each). Its cache starts with a 1.x compiler's format 1.0 file of
   `es3old` (`m8-1/build.sh`, from `~/ogles2-build/v3dglslc`), which the
   library refuses ("a GLSL ES 3.x program in a format 1.0 file") and
   replaces with a format 2 one that the later steps link
   (`ogles2-m8-1-20261023-143820`). gl4es with the regression set (§3).

Done when: GLSL ES 3.00 programs compile, link and draw in an ES3 context;
dEQP-GLES2, the GLES2 regression set and the speed figures unchanged.

**Stage A's end (2026-10-23, on the A1200; all but dEQP-GLES2):**
- **dEQP-GLES2 postponed** (the developer's decision, 2026-10-23): its
  17,165 cases take about 4 hours; it runs once the ES3 library is
  finished. `m7-1`'s `info` passes 6 of 6 with this work's library
  (`ogles2-m7-1-20261023-135452`).
- **The GLES2 regression set**, built from this copy (release and sync
  builds of `gles3` at ecfbeae), each against its last 1.x run: `m5-1`,
  `m6-6`, `m6-8`, `m6-10`, `m7-2`, `m7-3`, `m7-4` every check as before;
  the corpus `m2-5` 14 of 14 against llvmpipe's references
  (`tests/corpus/compare.py`); SDL2 `m6-3` and SDL3 `m6-4` the same output
  (pointer values aside; SDL3's extension list now has 1.3's
  `EXT_color_buffer_half_float`), frame rates no lower; gl4es `m6-1`
  (gears 731, 355, 452 and 1095 fps against 706, 333, 412 and 1044) and
  Quake III through gl4es `m6-2` ran; Quake III `m6-5` drew its demo; the
  release's four examples from `PROGDIR:shaders/`, nothing compiled (a
  drawer of the release build's `Examples/` with this library;
  `ogles2-ex-20261023-151615`). Results: `tests/a1200/results/*-20261023-14*`.
- **Speed, against 1.3.1 the same day, with 1.x's binaries** (a binary
  linked against this work's `libogles2.a` refuses 1.3.1: its dispatch
  table is larger, the import library's size check, as intended; so the
  comparison runs 1.x's Quake III and glmark2, of 2026-10-14/15, on each
  library): Quake III's timedemo `four` 88.0 and 87.1 fps against 88.3
  and 88.3 (about 0.9% slower), glmark2 251, 258, 257 against 260, 251,
  260 (means 255.3 and 257.0, 0.7%; runs of one library differ by 9). Both
  within the 2% (§3). Packages `ogles2-m6-5-new`/`-131`, `ogles2-m6-7-new`/
  `-131` (copies of `m6-5` and `m6-7` with the library swapped,
  `tests/a1200/out/`, not in git).
- **dEQP-GLES3, the groups of stage A's features** (package `m8-2`, with
  `ports/deqp/patches/gles3/0001`: a state reset that fails after a case is
  logged instead of ending the run): `info` 6/6, `shaders.fragdata` 8/8,
  `function` 4/4, `negative` 12/12, `discard` 25/25, `return` 38/38,
  `conditionals` 41/41, `builtin_variable` 5/29 (`vertex_id`, `frontfacing`,
  `pointcoord`, `depth_range` pass; 22 fail setting up the shader
  executor's framebuffer, `GL_INVALID_ENUM` at `glsShaderExecUtil.cpp:670`:
  its sized formats, stage B; `fragcoord_xyz` and `fragcoord_w` fail their
  image comparison, **not looked at yet**). Stopped there (the developer):
  `declarations`, `qualification_order`, `linkage` and `switch` not run.
  Runs `ogles2-m8-2-20261023-1517*` to `-151958`.

## 5. Stage B: the ES 3.0 core that saves CPU (5-6 weeks)

The 68k CPU is the limit in most programs; these features cut its work. Each
is offered in ES3 contexts and, where §1 names its GLES2 extension, in GLES2
contexts as that extension (the 2.0 release), with the enums, queries and
formats that extension's specification adds (`gl.xml`):
`VERTEX_ATTRIB_ARRAY_DIVISOR_EXT`, `VERTEX_ARRAY_BINDING_OES`,
`TEXTURE_BINDING_3D_OES`, `MAX_3D_TEXTURE_SIZE_OES`,
`FRAMEBUFFER_ATTACHMENT_TEXTURE_3D_ZOFFSET_OES`,
`FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING_EXT`, `SRGB_EXT` and
`SRGB_ALPHA_EXT`, `TEXTURE_IMMUTABLE_FORMAT_EXT` and `EXT_texture_storage`'s
sized formats for what GLES2 contexts have (`ALPHA8_EXT`,
`LUMINANCE8_EXT`, `LUMINANCE8_ALPHA8_EXT`, the 16F and 32F ones of
`OES_texture_*float`, `BGRA8_EXT`, R8 and RG8 with `EXT_texture_rg`); in
ES3 contexts the core names, with `VERTEX_ATTRIB_ARRAY_INTEGER`,
`VERTEX_ATTRIB_ARRAY_DIVISOR`, `TEXTURE_IMMUTABLE_FORMAT` and
`TEXTURE_IMMUTABLE_LEVELS` (dEQP-GLES3's `state_query.shader` and
`state_query.texture`).

1. **GPU copies of buffer objects** (first: item 6 and stages C and D need
   them). A buffer gets a GPU copy in the GPU's byte order, made
   when the GPU first needs the buffer itself (a uniform buffer binding,
   later transform feedback, storage and atomic counter bindings) and kept
   up to date as `buffer.c` keeps its copies now (renamed while a job uses
   it, patched otherwise). Every use that reads or writes it on the GPU
   does so in 32-bit words (std140 and std430 have no smaller types in GLSL
   ES 3.x; transform feedback writes 32-bit words; indirect commands are
   read on the CPU, stage D item 3), so the copy is the CPU copy swapped
   per 32-bit word; the data cache clean before every submission
   (`clean_all`, `job.c:680-683`, called at `:883`) covers it, as it covers
   every block the CPU writes for the GPU now. 16- and 8-bit data
   (indices, vertex attributes) would be
   scrambled by that swap; they keep going through the converted copies.
   A buffer the GPU wrote (transform feedback, storage, atomics) has its
   GPU copy current: **any CPU access** first waits for the job, calls
   `CachePostDMA` on the copy (as `display.c` does before reading the GPU's
   pixels) and swaps it back into the CPU copy: reads (a read mapping,
   `glCopyBufferSubData` from it, a pixel unpack, a vertex or index use)
   and writes too (`glBufferSubData`, a write mapping, a copy or pixel pack
   into it), since `patch_copies` would otherwise drop the GPU's results
   (`buffer.c:351-381`). The same waiting and `CachePostDMA` apply to every
   other value the CPU reads from the GPU: occlusion counters and indirect
   counts (transform feedback's primitives are counted on the CPU, stage
   C). A block the GPU writes is padded to whole
   64-byte cache lines, so no cache line is shared with other data
   (blocks are aligned but not padded now, §2).
2. **Buffer objects in full:** the ES 3.0 targets (`GL_COPY_READ_BUFFER`,
   `GL_COPY_WRITE_BUFFER`, `GL_PIXEL_PACK_BUFFER`, `GL_PIXEL_UNPACK_BUFFER`,
   `GL_TRANSFORM_FEEDBACK_BUFFER`, `GL_UNIFORM_BUFFER` beside the two of
   now) and usages (`*_READ`, `*_COPY` beside `*_DRAW`) as bindings
   (dEQP-GLES3's `buffer.write` uses all of them, `es3fBufferWriteTests.cpp`);
   and references: a buffer stays alive while a vertex array object, an
   indexed binding or a transform feedback object holds it, after
   `glDeleteBuffers` unbinds it from the current context (ES 3.0 appendix
   D; `lifetime.attach.deleted_input.buffer_vertex_array`), as textures'
   orphans do now, in both context versions. 1.3 frees it at once (§2).
   **Targets done (2026-10-25, on the A1200):** the six ES 3.0 targets and
   their binding queries, the nine usages (the static ones keep GPU
   copies, the others are read from the shadow as `GL_DYNAMIC_DRAW` is),
   every data call on any target (`slot` in `buffer.c`); the indexed
   bindings (`glBindBufferBase`, `glBindBufferRange` with Mesa's checks,
   `glGetIntegeri_v`, `glGetInteger64i_v`; 108 uniform and 4 transform
   feedback ones, which also set the target's binding), held for item 6
   and stage C; `glCopyBufferSubData` (of item 12) on the shadows, its
   copies patched. The pixel buffers work now rather than in stage C item
   3, since a bound one makes an application's pointer an offset: the
   uploads (`glTexImage2D`/`3D`, the sub-images, the compressed ones)
   read from the unpack buffer, `glReadPixels` writes into the pack buffer
   (`ogles2_buffer_pixels`; past the end or mapped, `GL_INVALID_OPERATION`),
   and the library's own images and reads set it aside
   (`OGLES2PixelStore.own`). Deleting unbinds a buffer from all of them in
   every member. Not yet: the references that keep a deleted buffer alive.
   `m8_es3`'s targets, bindings, ranges and errors (under vamos without
   `glBufferData`: it times itself on the Pi's system timer, which vamos
   lacks); `m8_draw`'s texel from an unpack buffer, the pixel read into a
   pack buffer and back, texels copied between buffers
   (`ogles2-m8-1-20261025-001318`: 24, 82, 37 of 37; the GLES2 drawing
   tests as before, `ogles2-m7-4-20261025-001342`).
3. **Vertex array objects** (`glBindVertexArray` and friends;
   `OES_vertex_array_object`): `ctx->array[16]` and the element buffer binding
   move into the object, kept in ES 3.1's form from the start: 16 attribute
   formats and 16 buffer bindings with their strides and divisors (Mesa's
   `gl_array_attributes` and `gl_vertex_buffer_binding`),
   `glVertexAttribPointer` and `glVertexAttribDivisor` setting both, so stage
   D's vertex attribute binding adds only its API
   (`vertex_attribute_binding.usage.mixed_usage` mixes the two). Client-side
   arrays are refused while a vertex array object other than 0 is bound:
   `GL_INVALID_OPERATION` from `glVertexAttribPointer` when no buffer is bound
   and the pointer is not NULL, in both context versions (ES 3.0 and
   `OES_vertex_array_object`'s issue "Should a vertex array object be allowed
   to encapsulate client vertex arrays? RESOLVED: No."); object 0 keeps them,
   so GLES2 programs are unchanged. The dEQP port's patch 0004 can then go.
   **Done in ES3 contexts (2026-10-25, on the A1200), not yet in ES 3.1's
   form:** `glGenVertexArrays`, `glDeleteVertexArrays`, `glIsVertexArray`
   (once bound), `glBindVertexArray` (names `glGenVertexArrays` gave),
   `GL_VERTEX_ARRAY_BINDING`, in `vao.c`. An object holds the 16 arrays as
   the context does (`struct OGLES2Array`) and the element buffer; the bound
   one's state is the context's own, so binding saves the old one's and
   loads the new one's and the draw path is unchanged (object 0's waits
   meanwhile). Stage D's attribute bindings will split the arrays into
   formats and bindings then. Client arrays are refused in an object but
   0. A deleted buffer is unbound from the context's other objects too
   (ES 3.0 keeps it alive there: item 2's references). Not yet:
   `OES_vertex_array_object` in GLES2 contexts (and with it dropping patch
   0004). `m8_es3`'s state and errors, `m8_draw`'s two objects with their
   own buffers, one with an element buffer (`ogles2-m8-1-20261025-141310`:
   25, 89, 44 of 44; the GLES2 drawing tests as before,
   `ogles2-m7-4-20261025-141334`). dEQP-GLES3's `vertex_array_objects`
   10 of 10 (`ogles2-m8-2-20261025-141411`), and with items 2 and 12's
   targets, usages, mappings and copies `buffer` 695 of 695
   (`-141428`).
4. **Instancing:** `glDrawArraysInstanced`, `glDrawElementsInstanced`,
   `glVertexAttribDivisor` (the `VERTEX_ARRAY_INSTANCED_PRIMS` and
   `INDEXED_INSTANCED_PRIM_LIST` packets, the attribute record's divisor;
   `EXT_instanced_arrays`). The per-draw attribute copies cover, for an
   attribute with a divisor, round-up(instances / divisor) elements instead
   of the index range. The record's divisor field is 16 bits (Mesa clamps
   larger divisors to 0xffff, `v3dx_state.c:388`); a divisor at or above
   the instance count is the same as one equal to it, so only a draw of more
   than 65535 instances with a larger divisor differs, a deviation
   `ogles2.doc` lists.
   **Done in ES3 contexts (2026-10-25, on the A1200):** the three entries
   and `GL_VERTEX_ATTRIB_ARRAY_DIVISOR`, as above (`draw.c`: an array's
   `divisor`, the source's and the record's, `draw_packets`' instanced
   forms, packets 34 and 38, which v3dcore has no writers for; the packet
   check compares a record with a divisor too). An instanced array is not
   moved back for `gl_VertexID`; the library's own draws clear the
   divisor of the arrays they borrow. Not yet: `EXT_instanced_arrays` in
   GLES2 contexts. `m8_draw`'s `es3inst` (`gl_InstanceID` moves instance i
   right by i): two instances, a divisor of 2, then none
   (`ogles2-m8-1-20261025-142206`: 25, 89, 47 of 47; the GLES2 drawing
   tests as before, `ogles2-m7-4-20261025-142237`). dEQP-GLES3's
   `instanced` 37 of 45: the 8 others are integer attributes, item 5
   (`ogles2-m8-2-20261025-142314`).
5. **Vertex formats:** integer attributes (`glVertexAttribIPointer`,
   `glVertexAttribI4*`: the attribute record's "read as int/uint"), and ES
   3.0's `GL_HALF_FLOAT`, `GL_INT`, `GL_UNSIGNED_INT` and
   `GL_INT_2_10_10_10_REV`/`GL_UNSIGNED_INT_2_10_10_10_REV` attributes
   (`copy_array` and `attr_record` in `draw.c`). Unsigned 32-bit values read
   as float are converted on the CPU: the hardware has no such conversion
   (Mesa lists none and has `u_vbuf` convert them). Three-component half
   floats are widened to 32-bit floats on the CPU, as `u_vbuf` does (Mesa
   never gives them to the hardware; whether it could read them is
   unverified). The current values of disabled attributes become typed
   (float, int or uint, as the last `glVertexAttrib*` call gave them):
   `constant_attrib`'s record (`draw.c:794-808`) gets the INT type with
   "read as int/uint" and the raw words for an integer one; and
   `default_attribs`' block (`:608-624`), which supplies components an
   array lacks, gives an integer input slot the integer 1 in w (Mesa does
   the same, `v3dx_state.c:440-456`), which makes that block per program
   rather than per control list.
   **Done in ES3 contexts (2026-10-25, on the A1200):**
   `glVertexAttribIPointer` (an array's `integer`, the record's bit 39),
   `glVertexAttribI4i`/`ui`/`iv`/`uiv` (the state's `attrib_kind` and
   `attrib_word`; `constant_attrib` writes the words with type INT, read
   as integers), `glGetVertexAttribIiv`/`Iuiv`,
   `GL_VERTEX_ATTRIB_ARRAY_INTEGER`; the five types in
   `glVertexAttribPointer` (2_10_10_10 of 4 components only). Half floats
   and 32-bit integers read as integers are swapped for the hardware's
   types; signed as well as unsigned 32-bit integers read as floats are
   converted on the CPU (simplest, and the hardware's conversion of the
   signed ones is unverified too), and so are three half floats
   (`CONV_*`, as `convert_elems`' type, which keeps a buffer's copies of
   each apart; `ogles2_buffer_copy` takes the converted stride). The
   default values: a block of the draw's own with the integer 1 in each
   integer array's w, when there is one, rather than per program.
   `m8_draw`'s `es3int`: two-component `GL_INT` and `GL_SHORT` integer
   arrays (w the integer 1), an unsigned current value, three half floats,
   normalized unsigned ints, a 2_10_10_10 word
   (`ogles2-m8-1-20261025-180052`: 25, 89, 51 of 51; the GLES2 drawing
   tests as before, `ogles2-m7-4-20261025-180123`). dEQP-GLES3 with items
   3 to 5: `instanced` 45 of 45, `default_vertex_attrib` 80 of 80,
   `attribute_location` 298 of 298, `vertex_arrays` 1005 of 1005
   (`ogles2-m8-2-20261025-1802xx` to `-180408`).
6. **Uniform buffers:** `glBindBufferBase/Range`, `glUniformBlockBinding`,
   `glGetUniformBlockIndex`, `glGetActiveUniformBlock*`, `glGetUniformIndices`,
   `glGetActiveUniformsiv`, `glGetIntegeri_v`; `QUNIFORM_UBO_ADDR` points into
   the GPU copy (item 1) of the buffer bound to the block that the stage's
   block map names (§3).
   **Items 1 and 6 done (2026-10-25, on the A1200):** a buffer's GPU copy
   (`ogles2_buffer_words`): every 32-bit word swapped into the GPU's order,
   padded to whole cache lines, made at the first use, kept with the data
   serial; a `glBufferSubData` swaps the words it touches again unless a
   job not yet rendered reads the copy, which then goes with that job and
   is made anew at the next use (as the draws' copies are renamed). Not
   yet: the GPU-written side (the CPU waits and `CachePostDMA`), with
   stage C's transform feedback. A program keeps each block's binding
   (`block_binding`, from the image's at link; `glUniformBlockBinding`);
   `glGetUniformBlockIndex`, `glGetActiveUniformBlockiv` and `_Name`,
   `glGetUniformIndices`, `glGetActiveUniformsiv` (the default block's
   offsets and strides -1, as Mesa's), `GL_ACTIVE_UNIFORM_BLOCKS` and its
   name length. A draw's `QUNIFORM_UBO_ADDR` of block k of a stage (Mesa's
   UBO k + 1) reads the range bound at that block's binding, or the zeroed
   block without one (`block_buffer`); `QUNIFORM_GET_UBO_SIZE` is the
   range's bytes (Mesa's `buffer_size`; data 0 the constant buffer: it
   took the block's size by the wrong index before). `m8_draw`'s `Extra`
   block: its queries, a range at binding 3, two draws of one frame either
   side of a `glBufferSubData`, zeros again once the buffer is deleted
   (`ogles2-m8-1-20261025-003343`: 24, 84, 41 of 41 in each drawing step;
   the GLES2 drawing tests as before, `ogles2-m7-4-20261025-003422`).
   dEQP-GLES3: `shaders-uniform_block` 138 of 138
   (`ogles2-m8-2-20261025-003501`); `ubo` passed its first 1387 cases,
   then the A1200 stopped in the 1388th,
   `ubo.single_nested_struct_mixed_matrix_packing.per_block_buffer.
   shared_block_row_major_matrix_row_major_matrixarray_row_major_instance_array_vertex`,
   after one shader compiled and before its program did (the on-target
   compiler, it seems; the case's shaders are not in the log, so not
   reproduced on the PC yet) and needed a restart
   (`ogles2-m8-2-20261025-003529`, the files fetched from the A1200).
   **Open:** that case's program through the host compiler, then the rest
   of `ubo` (from case 1389: `--deqp-caselist` of the remainder).
7. **Sampler objects** (`glGenSamplers`, ...): the sampler state record per
   unit from the sampler object when one is bound (`ogles2_texture_state`).
   The texture's cached records (`texture.c:2765-2771`) are keyed on the
   bound sampler object and its version too; filters, completeness and the
   forced-nearest rule read the sampler's parameters, not the texture's
   (dEQP-GLES3's `samplers` draws with a sampler, unbinds it and draws
   again with the texture unchanged, `glsSamplerObjectTest.cpp:520-541`).
   **Done (2026-10-24, on the A1200):** `glGenSamplers`, `glDeleteSamplers`,
   `glIsSampler`, `glBindSampler`, `glSamplerParameter*`,
   `glGetSamplerParameter*` and `GL_SAMPLER_BINDING` (ES3 contexts; in
   `texture.c`). A texture's sampling parameters are a `struct
   OGLES2SamplerParams` (filters, the three wraps, LODs, anisotropy, compare
   mode and function), a sampler object's too; `smp_param` and `smp_get`
   serve both (`GL_TEXTURE_LOD_BIAS`, the levels and the swizzle are the
   texture's only). `ogles2_texture_state` reads the unit's sampler
   object's, for completeness too, and the records are checked against its
   version (`st_sver`; versions unique across samplers). The names are
   the share group's, from `glGenSamplers` only (a plain table); deleting
   one unbinds it in every member of the group (ES 3.0 keeps it alive in
   another context while bound there: a deviation). A copy's draw sets
   unit 0's sampler object aside (`ogles2_texture_unit0`). `m8_es3`'s
   state and error checks, `m8_draw`'s sampler that completes a texture
   and one that makes it incomplete (`ogles2-m8-1-20261024-233802`: 20,
   62, 24 of 24 in each drawing step); the GLES2 drawing tests as before
   (`ogles2-m7-4-20261024-233832`).
8. **Textures:** immutable storage (`glTexStorage2D/3D`;
   `EXT_texture_storage`), 3D and 2D array textures (`glTexImage3D`,
   `glTexSubImage3D`, `glCopyTexSubImage3D`, `glCompressedTexImage3D`;
   `OES_texture_3D`: new texture kinds and unit bindings, the sampler's wrap
   R), the sized internal formats of ES 3.0 (R and RG: `EXT_texture_rg`;
   integer, 8-bit signed normalized (sampled only), sRGB, 10:10:10:2,
   11:11:10, 9:9:9:5, `DEPTH_COMPONENT24`, `DEPTH_COMPONENT32F` and
   `DEPTH32F_STENCIL8`, whose stencil Mesa keeps in a buffer of its own) and
   their upload types: `GL_HALF_FLOAT` (0x140B; `OES_texture_half_float`'s
   `HALF_FLOAT_OES` is 0x8D61, and both are accepted where each applies),
   the packed `GL_UNSIGNED_INT_2_10_10_10_REV`, `_10F_11F_11F_REV`,
   `_5_9_9_9_REV`, `_24_8` (swapped per 32-bit word from the 68k's order,
   as all packed types are) and `GL_FLOAT_32_UNSIGNED_INT_24_8_REV` (two
   words a texel). The RAM copies (`texture.c`'s copy classes RGBA8, FLOAT,
   DEPTH16, DEPTH24, BLOCKS) gain classes for these formats. Texture
   swizzle, `GL_TEXTURE_COMPARE_MODE` and `_FUNC` (shadow samplers: the
   sampler's compare function, fixed to NEVER now), the unpack parameters
   ES 3.0 adds (`UNPACK_ROW_LENGTH`, `_IMAGE_HEIGHT`, `_SKIP_ROWS`,
   `_SKIP_PIXELS`, `_SKIP_IMAGES`, with their queries; 1.3 has the
   alignments only, `texture.c:670-689`).
   Completeness in ES3 contexts by ES 3.0's rules (§3.8.13): the mip chain
   measured from `GL_TEXTURE_BASE_LEVEL`, not level 0, with ES 3.0's rules
   for base and max level on immutable textures; 32-bit float, integer and
   depth textures (depth without compare mode) with a linear filter are
   incomplete, not sampled nearest as in GLES2 contexts, and a depth
   texture with compare mode on filters as set (dEQP-GLES3's
   `texture.shadow.*linear*`). `glGenerateMipmap` of 3D textures on the CPU
   (the TFU makes 2D levels). Cube maps: ES 3.0 requires the seamless
   filtering V3D always does, so 1.3's deviation is correct behaviour in
   ES3 contexts.
   **Swizzle and compare done (2026-10-24, on the A1200):**
   `GL_TEXTURE_SWIZZLE_R/G/B/A`, `GL_TEXTURE_COMPARE_MODE` and `_FUNC`, and
   `GL_TEXTURE_WRAP_R` (ES3 contexts; `GL_INVALID_ENUM` in GLES2 ones). The
   swizzle composes with the stored format's (Mesa's
   `util_format_compose_swizzles`); with the compare mode on, the sampler
   record has the compare function and a depth texture filters as set
   (not forced nearest). A copy's draw (`ogles2_texture_params_copy`) uses
   the identity swizzle and no comparison. `m8_draw`'s (b, 0, 1, r)
   swizzle; the shadow samplers' drawing waits for the depth formats of
   this item.
   **2D array and 3D textures done (2026-10-25, on the A1200):**
   `GL_TEXTURE_2D_ARRAY` and `GL_TEXTURE_3D` bindings (and their queries),
   parameters, `glTexImage3D`, `glTexSubImage3D`, `glGenerateMipmap`, and
   `sampler2DArray`, `sampler3D` and the shadow samplers in programs
   (`OGLES2_TEX_ARRAY`, `OGLES2_TEX_3D`; `OGLES2_TEX_NONE` is left to the
   integer samplers), in ES3 contexts. A texture's faces are a pointer
   (`f6`, or allocated past six): an array's layers and a 3D texture's
   slices are faces, `d[l]` their number at level l (a 3D level has
   `d[l]` slices); completeness checks each level's depth (`depth_ok`) and
   a 3D chain's length by its depth too (`chain_levels`). The GPU copy as
   Mesa lays it out: an array as a cube map with N layers (`cube_memory`),
   a 3D texture each level's slices together, smallest level first, from
   level 1 on the depth of the power-of-two chain (`volume_memory`, Mesa's
   `v3d_setup_slices`; the texture state's array stride is level 0's
   slice size); `face_gpu` gives a slice of a level. 3D mipmaps are made
   on the CPU, each slice two of the level above. A depth comparison makes
   a sampler's return 16 bits (`ogles2_texture_wide`, as Mesa's
   `v3d_get_tex_return_size`). Not yet: `glCopyTexSubImage3D`,
   `glCompressedTexImage3D`, layers as framebuffer attachments (item 11),
   immutable storage. `m8_draw`'s layers of a 2x2x4 array (and one
   replaced), slices of a 2x2x4 and a 64x64x8 3D texture (UIF levels) and
   their `glGenerateMipmap` levels, through `es3layer` and `es3vol`;
   `m8_es3`'s errors (`ogles2-m8-1-20261024-235209`: 22, 71, 31 of 31).
   **The pixel store done (2026-10-25, on the A1200):** `GL_UNPACK_ROW_LENGTH`,
   `_IMAGE_HEIGHT`, `_SKIP_PIXELS`, `_SKIP_ROWS`, `_SKIP_IMAGES`,
   `GL_PACK_ROW_LENGTH`, `_SKIP_PIXELS`, `_SKIP_ROWS` (ES3 contexts, with
   their queries; `struct OGLES2PixelStore`, `ctx->unpack` and
   `ctx->pack`): the uploads read the application's image through
   `unpack_image` (the converters take a row stride), `glReadPixels`
   packs so (`frame.c`), and the library's own tight images go through
   `unpack_tight`. `glPixelStorei` checks the name before the value (a
   new name with 0 was `GL_INVALID_VALUE`). `m8_draw`'s texel from inside
   a 4x3 image, layers from images with a gap, a read into the middle of a
   larger buffer (`ogles2-m8-1-20261024-235930`: 23, 75, 34 of 34; the
   GLES2 drawing tests as before, `ogles2-m7-4-20261024-235957`); the
   state reset's pixel transfer part then stops at the buffer targets
   (`ogles2-m8-2-20261025-000023`).
   **Immutable storage for the stored formats done (2026-10-25, on the
   A1200):** `glTexStorage2D` and `glTexStorage3D` (`tex_storage`) for the
   sized formats the library stores (`sized_storage`: RGBA8, RGB8, RGBA4,
   RGB5_A1, RGB565, RGBA/RGB16F and 32F, `DEPTH_COMPONENT16`/`24`,
   `DEPTH24_STENCIL8`): every level of every face made blank, in the format
   and type such a level is kept in; `immutable_levels`, with
   `GL_TEXTURE_IMMUTABLE_FORMAT` and `_LEVELS`; ES 3.0's clamping of base
   and max level to the levels (`complete`); glTexImage* of such a texture
   is `GL_INVALID_OPERATION`. ES 3.0's other sized formats are refused
   with `GL_INVALID_OPERATION` until they are stored (`es3_sized`), the
   unsized with `GL_INVALID_ENUM`. glTexImage* take the float and
   `DEPTH_COMPONENT24` sized formats too (`sized_base`), and ES 3.0's
   `GL_HALF_FLOAT` as `GL_HALF_FLOAT_OES` (`half_type`). A texture given
   its images in an ES3 context samples depth as ES 3.0's (d, 0, 0, 1)
   (`es3_rules`; GLES2's `OES_depth_texture` keeps (d, d, d, 1)), and ES3
   contexts take depth cube maps. `m8_es3`'s storage state and errors,
   `m8_draw`'s two levels by sub-images at base levels 0, 1 and 5
   (clamped) (`ogles2-m8-1-20261025-190352`: 25, 100, 61 of 61; the GLES2
   drawing tests as before, `ogles2-m7-4-20261025-190402`); dEQP-GLES3's
   `texture.specification.texstorage*` 74 of 240: every case of the stored
   formats and every size case; the other 166 are formats not stored yet
   (`ogles2-m8-2-20261025-190429`).
   **R and RG (2026-10-25, on the A1200):** `GL_RED` and `GL_RG` (ES3
   contexts only, `rg_refused`: `EXT_texture_rg` is not offered in GLES2
   ones) with unsigned bytes, half floats and floats, and the sized `R8`,
   `RG8`, `R16F`, `RG16F`, `R32F`, `RG32F` (glTexImage*, glTexStorage*):
   the RAM copy holds (r, 0, 0, 1) and (r, g, 0, 1); stored as the
   luminance and alpha formats' hardware types (R8, RG8, R16F, RG16F,
   R32F, RG32F), told apart by the swizzle, (R, 0, 0, 1) and (R, G, 0, 1),
   which `pack`, `pack_wide` and `unpack` read (RG's second byte green,
   not alpha). `m8_draw`'s `GL_R8`, `GL_RG8`, `GL_R16F`, `GL_RG32F` texels
   (`ogles2-m8-1-20261025-191649`: 25, 100, 63 of 63; the GLES2 drawing
   tests as before, `ogles2-m7-4-20261025-191659`); dEQP-GLES3's
   `texture.specification.texstorage*` 98 of 240 and `texture.format.*` 206
   of 546, every failure a format not taken yet, none an image
   (`ogles2-m8-2-20261025-191749`, `-191827`). Rendering into them
   is item 10's.
   **Signed normalized, sRGB, packed and 32F depth (2026-10-25, on the
   A1200):** `R8_SNORM` to `RGBA8_SNORM` (`GL_BYTE`), `SRGB8` and
   `SRGB8_ALPHA8`, `RGB10_A2` (`UNSIGNED_INT_2_10_10_10_REV`),
   `R11F_G11F_B10F` and `RGB9_E5` (their packed types, half floats or
   floats), `DEPTH_COMPONENT32F` and `DEPTH32F_STENCIL8` (`GL_FLOAT`,
   `FLOAT_32_UNSIGNED_INT_24_8_REV`; the depth only, as sampling sees
   it), in glTexImage* and glTexStorage* (ES3 contexts; `rg_refused` keeps
   GLES2 contexts' errors). The signed and packed ones keep a float RAM
   copy (`convert_float`, `unpack_word`) the store encodes per hardware
   type (`pack_wide`, `encoded_type`: the 11- and 10-bit floats as a half
   float's top bits, RGB9_E5 by ES 3.0's equations 3.4 to 3.7 without
   libm, `rgb9e5`); sRGB keeps its bytes and the texture state's sRGB bit;
   32F depth its float's bits (the TMU's `DEPTH_COMP32F`). A level keeps
   the sized format where format and type do not say the storage
   (`OGLES2Face.sized`: sRGB, the packed floats given as floats), which
   also keeps those from the 32-bit variants (`level_wide`). `m8_draw`'s
   texel of each (`ogles2-m8-1-20261025-192536`: 68 of 68; the GLES2
   drawing tests as before, `ogles2-m7-4-20261025-192608`); dEQP-GLES3's
   `texstorage*` 140 of 240 and `texture.format.*` 290 of 546, the rest
   all integer formats (`ogles2-m8-2-20261025-192636`, `-192748`).
   **Integer formats (2026-10-25, on the A1200):** the `*_INTEGER` formats
   with 8-, 16- and 32-bit types and `RGB10_A2UI`, sized only
   (`int_sized`; an unsized one is `GL_INVALID_VALUE`): a RAM copy of four
   32-bit words a texel (`convert_int`, signed types sign-extended), the
   TMU's integer types (RGB as RGBA, alpha 1) narrowed into by
   `pack_wide`; the 32-bit ones return 32 bits (`wide_class`, Mesa's
   table), the others 16 as the plain variants read. The integer samplers
   are their dimension's kind with `OGLES2_TEX_INT` (`ogles2_sampler_kind`;
   `OGLES2_TEX_NONE` is left to samplers of no kind), and an incomplete
   integer texture reads `fallback_int`, (0, 0, 0, 1) in RGBA8UI; ES 3.0
   3.8.13's integer texture with a linear filter is incomplete; no mipmaps
   of them (`GL_INVALID_OPERATION`). `m8_draw`'s `es3utex` and `es3itex`:
   RGBA8UI, RGBA32UI, R16I, one filtered linearly
   (`ogles2-m8-1-20261025-193512`: 72 of 72; the GLES2 drawing tests as
   before, `ogles2-m7-4-20261025-193542`). dEQP-GLES3's
   `texture.specification.texstorage*` 240 of 240, `texture.format.*` 490
   of 546 and no failure (56 not supported), `ogles2-m8-2-20261025-193607`,
   `-193718`. The whole `texture` group then: 4077 of 5895, 24 failing
   (`ogles2-m8-2-20261025-193925`): glTexImage3D lacked glTexImage2D's
   `GL_BGRA8_EXT` name, and ES 3.0 clamps depth components to [0, 1] when
   given (3.7.2), floats too; both fixed, the 24 pass
   (`ogles2-m8-2-20261025-204518`): 4101 of 5895 and no failure, the 1794
   not supported compressed (ETC2/EAC, item 9; ASTC, not ES 3.0's).
   **Trap:** that run's `Execute Run-M1` did not return although the
   script had ended: the compiler server (started by the library, ended
   by its expunge) was still running; `Avail FLUSH` ended it, but the PC's
   `runpkg` still waited, and the results came with `amictl get -r` (the
   15 MB in 7 s). Left in this item: `glCopyTexSubImage3D`
   (`glCompressedTexImage3D` came with item 9), ES 3.0's completeness of base levels beyond
   0 for mutable textures, the 3D textures' mipmaps from a base level.
9. **ETC2/EAC** (mandatory in ES 3.0): native in the TMU (texture types
   `RGB8_ETC2`, `RGB8_PUNCHTHROUGH_ALPHA1`, `RGBA8_ETC2_EAC`, `R11_EAC`,
   `SIGNED_R11_EAC`, `RG11_EAC`, `SIGNED_RG11_EAC`; sRGB by the texture
   state's sRGB bit), in ES3 contexts; `GL_COMPRESSED_TEXTURE_FORMATS` grows
   to 15. ETC1, now decoded on the CPU, is sampled natively too, in both
   (`RGB8_ETC2` decodes ETC1): the main plan's 1.x item "native ETC1
   sampling", done here unless `main` does it first.
   **Done (2026-10-25, on the A1200):** the ten ETC2/EAC formats in
   `glCompressedTexImage2D` and `glCompressedTexSubImage2D` (ES3 contexts;
   GLES2 ones `GL_INVALID_ENUM`), kept as their blocks like S3TC
   (`dxt_block_bytes` knows them all, `storage_of` gives the TMU's types:
   R11 (R, 0, 0, 1), RG11 (R, G, 0, 1), RGB8 alpha 1; the sRGB ones set the
   texture state's sRGB bit); `glCompressedTexImage3D` and
   `glCompressedTexSubImage3D` for 2D arrays (a 3D texture is
   `GL_INVALID_OPERATION`, ES 3.0 3.8.6); `GL_COMPRESSED_TEXTURE_FORMATS`
   15 in ES3 contexts (ETC1 first), 5 in GLES2 ones. ETC1 is no longer
   decoded: kept and sampled as `RGB8_ETC2` in both kinds of context, and
   `ogles2/etc1.c`, its host test and CI step are gone (still refused by
   the sub-image entries, as the extension says). `m8_draw`'s solid block
   of each format and a compressed array, a layer replaced
   (`ogles2-m8-1-20261025-211103`: 25, 100, 81 of 81; the smoke test
   passes again, §1); the GLES2 drawing tests as before
   (`ogles2-m7-4-20261025-211548`); every ETC1, ETC2 and EAC case of
   dEQP-GLES3 passes, 492 (`texture.format.compressed`, `texture.wrap`,
   `texture.compressed`, `negative_api`'s; the 322 not supported are ASTC,
   not ES 3.0's: `ogles2-m8-2-20261025-211131`), and dEQP-GLES2's 97 ETC1
   cases with the native sampling (`ogles2-m7-1-20261025-211834`).
10. **Render targets beyond 8 bits:** 16- and 32-bit float, integer, sRGB.
    - Per format the render target's internal type, bpp and clamp, the tile
      size from the largest bpp, the render-target count and MSAA
      (`v3d_choose_tile_size`), tile pools sized from the job's actual tile
      size, and the clear colour in up to three packets (Appendix B.3;
      `job.c`'s `build_render_fbo`). GitHub issue 5 did this for 8-bit and
      16F targets: per target bpp, type and clamp
      (`TileRenderingModeCFGColorEach`), parts 1 and 2, Mesa's tiles down to
      32x16 and pools for them (`fbo.c:1097-1116`, `:1053-1056`). Left:
      128-bit targets
      (part 3; two RGBA32F targets give 32x16 tiles, three or four 16x16,
      and MSAA two steps more, so the pools and the tile table go further),
      the integer and sRGB internal types and their clamps, MSAA.
    - Renderable formats (`fbo.c`): the ES 3.0 colour formats,
      `EXT_color_buffer_float` in ES3 contexts, `EXT_sRGB` in GLES2 contexts
      (`EXT_color_buffer_half_float` is there since issue 5), and the R8 and
      RG8 targets `EXT_texture_rg` makes renderable; renderbuffers in the
      sized ES 3.0 formats, `DEPTH_COMPONENT24` (also as `OES_depth24` in
      GLES2 contexts) and `DEPTH_COMPONENT32F` among them.
    - **Blending as Mesa does it** (Appendix B.3): when every bound colour
      target can be blended in the tile buffer (internal types 8 and 16F),
      the tile buffer blends, through the `BLEND_ENABLES` mask and a
      `BLEND_CFG` per target where they differ: Mesa's `blend_dst_alpha_one`
      makes the destination alpha factor ONE for targets whose format has
      no alpha (its alpha swizzle is 1, `v3dx_state.c:550-551`: R8, RG8,
      RGB8, RGB565, R16F, RG16F, R11F_G11F_B10F, ...). When any bound target
      cannot (32-bit float or integer), the fragment shader blends every
      target (the key's `software_blend`, stage A item 5) and the tile
      buffer blends none, even for an RGBA8 target beside it; integer
      targets are never blended (ES 3.0; the shader skips them).
    - Clears and colours: `glClearBuffer*` (integer values raw); in ES3
      contexts the clear and blend colours are kept unclamped and clamped
      only for fixed-point targets, as Mesa does (`clear.c`,
      `blend.c:771-791`, `get.c:1164-1172`), not when set as now; clears the
      tile buffer cannot do and the blit's textured quad (item 11) draw with
      built-in programs **precompiled for every target kind** (8-bit and 16F
      share variants; 32F, signed and unsigned integer and depth need their
      own), since no compiler may be present, with the occlusion counter off
      while they draw; `glCopyTex*`'s program (`copy_tex_v3dp.h`) as well,
      for copies into the new formats. A framebuffer mixing kinds (RGBA8
      beside R32F) gives every draw `software_blend` and every target's
      format and blend state in the key (`drv/v3d_program.c:693`,
      `:711-735`), which no precompiled variant matches: the built-ins draw
      with blending off, are compiled with `software_blend` 0 and matched
      on the kinds of the targets they write only (to be confirmed on the
      host compiler before item 10 is built: with blending off the two
      write the same values).
    - `glReadPixels` from every target, with each format's
      `GL_IMPLEMENTATION_COLOR_READ_FORMAT` and `_TYPE`: in ES3 contexts a
      float target reads as `GL_FLOAT` (half-float ones as clamped bytes
      only in 1.3, `read_half`); the pack parameters ES 3.0 adds
      (`PACK_ROW_LENGTH`, `_SKIP_ROWS`, `_SKIP_PIXELS`; dEQP-GLES3's
      `read_pixels` sets them, `es3fReadPixelsTests.cpp:341-348`).

    **Done (2026-10-26, on the A1200).** A colour target's format comes
    from one table (`texture.c`, `rt_table`, Mesa's
    `v3dX(get_internal_type_bpp_for_output_format)` and format table):
    output format, internal type and bpp (sRGB, RGB10_A2 and the small
    floats 16F at 64 bits, R16F and RG16F 64 too), clamp (integers to
    their range, sRGB to [0, 1]), kind, channels and bits, and the pipe
    format and swizzle Mesa's fragment key names it by
    (`ogles2_texture_rt`, `struct OGLES2RtFormat`). Colour-renderable:
    GLES2's as before; in ES3 contexts, given a sized internal format
    (ES 3.0 4.4.4; `OGLES2Face.sized_given`), R8, RG8, RGB10_A2,
    SRGB8_ALPHA8, the integer formats but RGB, and
    `EXT_color_buffer_float`'s floats with R11F_G11F_B10F (offered in ES3
    contexts); RGB16F too (`EXT_color_buffer_half_float`). Renderbuffers
    take ES 3.0's sized formats (`ogles2_texture_hidden_sized`, stored as
    glTexStorage2D stores them), `DEPTH_COMPONENT24`,
    `DEPTH_COMPONENT32F` and `DEPTH32F_STENCIL8` (D32F in the tile buffer,
    its stencil an S8 image of its own, as Mesa's separate stencil);
    `glRenderbufferStorageMultisample` (stored single-sampled until stage
    C, `GL_RENDERBUFFER_SAMPLES` what was asked, logged once) and
    `glGetInternalformativ` (4 samples, none for integer formats); a
    32-bit float depth texture as a D32F target. The job: each target's
    internal type, bpp and clamp, the widest target's bpp (128 for RGBA32F
    and the RGBA 32-bit integers) in Mesa's tile sizes down to 16x16, pools
    for them, and each target's clear value packed in its internal type
    (`pack_clear`, the three clear-colour packets; Mesa's
    `v3d_tlb_clear`), a target cleared by the tile buffer not loaded. Draws:
    the fragment key's `f32_color_rb` for 32-bit float and integer targets,
    and with such a target Mesa's software blending (`software_blend`, each
    drawn target's pipe format, swizzle and blend state in the key,
    `vk_targets`), the tile buffer then not blending; a target without
    alpha blends with a destination alpha of 1 (a `BLEND_CFG` of its own,
    Mesa's `blend_dst_alpha_one`). Clears: ES3 contexts keep the clear
    colour unclamped; `glClearBufferiv`, `uiv`, `fv`, `fi` through the tile
    buffer per target; a partial one draws the clear quad confined to its
    draw buffer, but in a framebuffer with a 32-bit float or integer target
    (below) every target's box is filled by the CPU (`ogles2_texture_fill`,
    sRGB encoded) and depth and stencil are drawn by a program that writes
    no colour (`tools/clear_zs`). Reads: the RAM copy of every renderable
    kind is read back from the GPU (`unpack_wide`); `glReadPixels` takes
    ES 3.0's pairs (RGBA/FLOAT, RGBA_INTEGER with INT or UNSIGNED_INT,
    RGB10_A2's packed type), each the buffer's implementation read format.
    Also: `GL_SRC_ALPHA_SATURATE` as a destination factor in ES3 contexts;
    GLES2 contexts refuse ES 3.0's sized formats in glTexImage2D again (as
    1.3; item 8 had let them through). Tests: `m8_draw`'s RGBA32F, RGBA8I
    and R32UI at once (drawn, read, cleared whole and in the scissor box,
    blended in the shader), R8, RG8, SRGB8_ALPHA8, RGB10_A2,
    R11F_G11F_B10F, R16F, RG8 blended with GL_DST_ALPHA, DEPTH32F_STENCIL8
    with glClearBufferfi, rendered 128x128, 129x117 and 99x128 textures
    sampled, an RGBA8 and an R32I target's partial clears beside each
    other (`ogles2-m8-1-20261026-103218`: 25, 100, 103 of 103 three ways);
    the GLES2 drawing tests as before; dEQP-GLES3 (`ogles2-m8-2-20261026-
    103256` to `-104859`): `read_pixels` 48 of 48, `pbo` 54 of 54,
    `color_clear` 19, `fragment_out` 940 of 940, `shaders-builtin_variable`
    29 of 29, `fragment_ops` 3165 of 3174 (the 9 left need item 11's
    framebuffer targets), `texture` 4501 of 5895 and no failure, `fbo` 1157
    of 2075 (blits, layers, invalidation, the draw and read bindings,
    `GL_DEPTH_STENCIL_ATTACHMENT`: item 11; multisampling: stage C),
    `negative_api` 268 of 314 (item 11, transform feedback, program binary
    and `glCopyTexSubImage3D` left); dEQP-GLES2 as before
    (`ogles2-m7-1-20261026-112045` to `-112703`: `fbo` 709 of 709 again,
    `texture` 783 of 807 with the 24 cube map cases of the documented
    deviation, `negative_api` 246, `read_pixels` 8, `fragment_ops` 1923 of
    1923). **A trap that cost a day:** a fragment shader
    writing 16-bit floats into a 32-bit float or integer target (here the
    GLES2 clear quad, in dEQP-GLES3's
    `fbo.render.recreate_color.tex2d_rgba32f_*`) leaves the A1200 drawing
    wrong pixels in every later program until it is switched off; a reboot
    does not help (bisected by groups with `m7-4` after each). The draw
    path now refuses any draw whose fragment variant does not write each
    drawn target at its width (logged once), so neither a GLES2 program nor
    the closest variant of an ES3 one without a compiler can do it. Left:
    GLES2 (format 1) programs cannot draw into 32-bit float or integer
    targets (they get no variant of the width: stage C's request codes);
    textures in `DEPTH32F_STENCIL8` hold the depth only (their stencil is
    not attachable); the blend colour is still clamped when set; the
    built-ins for 32-bit targets are the CPU's fill and the no-colour
    program rather than precompiled variants.
11. **Framebuffers:** separate draw and read framebuffer bindings
    (`GL_DRAW_FRAMEBUFFER`, `GL_READ_FRAMEBUFFER`), `glBlitFramebuffer` in
    Mesa's `v3d_blit` order (`v3d_tlb_blit_fast`, `v3d_tfu_blit`,
    `v3d_tlb_blit`, `v3d_stencil_blit`, then a textured quad,
    `v3d_render_blit`, for scaling, filtering and scissored blits; Appendix
    B.3 gives each one's conditions). The default framebuffer is no
    resource Mesa's paths can take (dEQP-GLES3's 106
    `fbo.blit.default_framebuffer.*` and the scissored blits into it): a
    blit from it goes through `copy.c`'s path (the job finished, the TFU
    into a scratch texture, sampled flipped, `copy.c:9-16`); into its
    colour, a quad drawn in its open job; into its depth and stencil, which
    are neither loaded nor stored but across a mid-frame flush
    (`zs_save`, `job.c:107-114`), the region is drawn into a framebuffer
    object of the context's own over that save and the next job loads it.
    Stencil blits that scale (`fbo.blit.depth_stencil.*_scale`) follow
    `v3d_stencil_blit` (`v3d_blit.c:165-238`): the source's stencil sampled
    as an RGBA8UI (packed with depth) or R8UI texture, the destination's
    stencil drawn as a colour target of that format writing R only, so
    depth/stencil storage (UIF without XOR, `fbo.c:12-15`) gets texture and
    render-target views; scaled depth blits draw `gl_FragDepth` from the
    source with the depth built-in. `glInvalidateFramebuffer` and
    `glInvalidateSubFramebuffer` (as `EXT_discard_framebuffer`),
    `glReadBuffer`, `glDrawBuffers`, `glFramebufferTextureLayer`, the
    `GL_DEPTH_STENCIL_ATTACHMENT` point, ES 3.0's attachment queries
    (`fbo.c:1288-1334`: the default framebuffer's `GL_BACK`, `GL_DEPTH` and
    `GL_STENCIL`; on an empty attachment `OBJECT_NAME` 0 and
    `GL_INVALID_OPERATION` for the rest; the new `*_SIZE`, `COMPONENT_TYPE`,
    `COLOR_ENCODING` and `TEXTURE_LAYER` names; dEQP-GLES3's
    `fbo.api.attachment_query_empty_fbo`), and ES 3.0's completeness rules
    in ES3 contexts (`fbo.c`'s `status_of`; dEQP-GLES3's checker,
    `es3fFboCompletenessTests.cpp:179-240`): attachments of different sizes
    are complete and drawing covers their common area (no
    `FRAMEBUFFER_INCOMPLETE_DIMENSIONS`); `FRAMEBUFFER_INCOMPLETE_MULTISAMPLE`
    when single- and multisampled attachments mix; depth and stencil
    attachments, if both present, must be the same image, else
    `FRAMEBUFFER_UNSUPPORTED` (1.3 allows a `DEPTH_COMPONENT16` and a
    separate `STENCIL_INDEX8`). GLES2 contexts keep GLES 2.0's rules.

    **Done in part (2026-10-26, on the A1200).** The draw and read
    bindings (`ctx->read_fb`; `GL_FRAMEBUFFER` both, `GL_DRAW_FRAMEBUFFER`
    and `GL_READ_FRAMEBUFFER` one each, in ES3 contexts; names
    glGenFramebuffers did not return are bound as GLES2 allows, as Mesa
    does for ES), each entry on its target's framebuffer; reads
    (glReadPixels, glCopyTex*, the implementation read format) put the read
    binding in the draw binding's place while they run
    (`ogles2_fbo_read_begin`, `_end`), so their code is unchanged;
    `GL_DEPTH_STENCIL_ATTACHMENT` (both points); glFramebufferTextureLayer
    (a 2D array's layer, a 3D texture's slice: `face_gpu`'s address, the
    attachment's `face` 16 bits); glInvalidateFramebuffer as
    EXT_discard_framebuffer for either binding, glInvalidateSubFramebuffer
    checked and kept (as the specification allows); ES 3.0's completeness
    (attachments of different sizes complete, drawing their common area;
    `INCOMPLETE_MULTISAMPLE` from renderbuffers' sample counts; depth and
    stencil one image or `UNSUPPORTED`) and attachment queries (the
    default framebuffer's `GL_BACK`, `GL_DEPTH`, `GL_STENCIL`, the
    depth-stencil point, an empty one's `GL_NONE` and 0, sizes, component
    type, colour encoding, layer); deleting an object detaches it from both
    bindings. glBlitFramebuffer (`blit.c`) does colour on the CPU for every
    format: the source rectangle read back as floats or integers (sRGB
    decoded; the default framebuffer's bitmap too), nearest or bilinear
    per destination pixel with flips, the scissor box, pixels whose sample
    is outside the read framebuffer left alone, each draw buffer written
    (`ogles2_texture_write`: the RAM copy, sRGB encoded, and the GPU copy;
    `ogles2_display_write` for the bitmap), with ES 3.0's errors (integer
    against float, integers or depth with `GL_LINEAR`). `m8_draw`'s read
    and draw bindings, the depth-stencil point, a layer and a slice drawn
    and sampled, invalidation errors, blits scaled, mirrored, RGBA32F
    within the scissor box and from the default framebuffer
    (`ogles2-m8-1-20261026-122633`: 116 of 116 three ways); dEQP-GLES3 `fbo`
    1963 of 2075 (was 1157; `ogles2-m8-2-20261026-122716`),
    `fragment_ops.scissor` 26 of 26 (`ogles2-m8-2-20261026-121255`),
    `negative_api` 272 of 314; dEQP-GLES2's `fbo` 709 of 709 and
    `negative_api` 246 of 246 (`ogles2-m7-1-20261026-121331`, `-121532`).
    **Then (2026-10-26, on the A1200):** depth and stencil blits on the CPU
    (`fbo.c`'s `ogles2_fbo_blit_zs`): the renderbuffers' UIF images read
    and written through a texture view (`v3d_texture_load`, `_store`), byte
    by byte (D24S8's depth its upper three bytes, its stencil the low one;
    `DEPTH32F_STENCIL8`'s stencil its own S8 image), nearest texel with
    flips; the default framebuffer's through the depth and stencil it keeps
    across a mid-frame flush (`job.c`'s `ogles2_job_zs_view`; its rows top
    down), so only once something was drawn or cleared that frame, and not
    with 4x MSAA (logged). A `DEPTH32F_STENCIL8` texture has its stencil
    too: an S8 image per level beside the depth (`texture.c`'s
    `ogles2_texture_s8_surface`, made at the first use as a job's stencil;
    texels given with `glTexImage2D` keep their depth only). Invalidation
    as ES 3.0 programs use it: a draw or clear after it in the same job is
    stored again (`ogles2_job_begin` clears the job's discard mask), and the
    default framebuffer's depth or stencil alone keeps both (one D24S8
    image). A CPU clear of a 32-bit float target renders every open job
    first (a texture attached to two framebuffers). dEQP's
    `fbo.msaa.renderbuffer_resize` read RGBA8 pixels as `uint32_t`, wrong on
    a big-endian machine: `ports/deqp/patches/gles3/0001`. `m8_draw` 120 of
    120 three ways (`ogles2-m8-1-20261026-172503`: a `DEPTH32F_STENCIL8`
    texture's stencil across a read-back and as the stencil only, a
    renderbuffer given new storage while attached); dEQP-GLES3 `fbo` 2032
    of 2075 and none failing (41 not supported: formats ES 3.0 does not make
    renderable, multisampled textures; 2 quality warnings in
    `completeness.samples`; `ogles2-m8-2-20261026-173058`), `fragment_ops`
    3174 of 3174 (`-173806`), `negative_api` 273 of 314 (`-173748`: the
    rest are transform feedback, program binaries, glCopyTexSubImage3D,
    and buffer targets of glTexImage*); dEQP-GLES2's `fbo` 709 of 709 and
    `negative_api` 246 of 246 (`ogles2-m7-1-20261026-175040`, `-175305`);
    `m7-4` after the runs. Left: multisampling (stage C), and
    the GPU paths for blits (Mesa's order above) where the CPU is slow.
12. **Smaller ones:** sync objects (`glFenceSync`, `glClientWaitSync`,
    `glWaitSync`: a fence is the library's job serial, `ctx->job_serial`, done
    when that job has rendered, not the GPU's 8-bit counters;
    `GL_SYNC_FLUSH_COMMANDS_BIT` flushes the open job), occlusion queries
    (`GL_ANY_SAMPLES_PASSED` and `_CONSERVATIVE`: the
    `OCCLUSION_QUERY_COUNTER` packet, read after the job; counters laid out as
    Mesa does, Appendix B.3; `GL_QUERY_RESULT_AVAILABLE` submits the job
    holding the query's last draw, as Mesa flushes before even a non-waiting
    check, `drv/v3d_query_pipe.c:234-240`), primitive restart (fixed index
    only: the draw packets' restart bit; the range scan skips the restart
    index, and the rebasing keeps it as the hardware's restart value of the
    index size, 32-bit 0xffffffff included), `glMapBufferRange` and
    `glFlushMappedBufferRange` (`EXT_map_buffer_range`), `glUnmapBuffer`,
    `glGetBufferPointerv`, `glCopyBufferSubData`, `glGetBufferParameteri64v`;
    `EXT_frag_depth` in GLES2 contexts (the extension string and its test: the
    compiler and the draw already handle depth writes). Until the query
    objects come, `glGetQueryiv(target, GL_CURRENT_QUERY)` answers 0 (no
    query can be active; `state.c`), and `GL_TRANSFORM_FEEDBACK_ACTIVE`
    and `_PAUSED` read false until stage C: the state reset asks for both.
    **Mapping done (2026-10-25, on the A1200):** `glMapBufferRange` (Mesa's
    checks and their order), `glFlushMappedBufferRange`, `glUnmapBuffer`,
    `glGetBufferPointerv`, `glGetBufferParameteri64v` and the mapping's
    `GL_BUFFER_ACCESS_FLAGS`, `_MAP_OFFSET`, `_MAP_LENGTH` (ES3 contexts).
    The mapping is the shadow, as `OES_mapbuffer`'s: what was written goes
    into the copies at the unmap, or at each flush with
    `GL_MAP_FLUSH_EXPLICIT_BIT`; `glBufferSubData` of a mapped buffer is
    `GL_INVALID_OPERATION` in ES3 contexts. `m8_es3`'s reads, writes, state
    and errors (on the A1200: they need `glBufferData`), `m8_draw`'s block
    written through a flushed mapping (`ogles2-m8-1-20261025-141310`).
    **Sync objects and occlusion queries (2026-10-25):** `sync.c`: a fence
    is the open job's serial, else the running render's, else signalled
    at once; signalled when no member has that job open or running;
    `glClientWaitSync` flushes the context's own job (with or without the
    flush bit: else it would wait forever) and waits any timeout out in
    full (another member's open job: `GL_TIMEOUT_EXPIRED`); `glWaitSync`
    validates only (the GPU runs jobs in order); the objects are the share
    group's, a list that checks a name before use. `query.c`: per context;
    the active occlusion query's counter address is in each job's binning
    list from its start (`ctx->oq_addr`, `job.c`) and at
    `glBeginQuery`/`glEndQuery` in the open one, off around the library's
    own copies and quad clears (`ogles2_query_pause`); the word zeroed at
    begin (a counter a job still writes is left to it), read after
    `CachePostDMA` once the query's last job has rendered;
    `GL_QUERY_RESULT_AVAILABLE` submits the context's open job first, as
    Mesa does. `GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN` begins and ends
    and answers 0 until stage C. `m8_draw`'s fences (unsignalled after a
    draw, flushed and waited for, signalled at once with nothing
    outstanding) and queries (a draw passes samples, a triangle off the
    picture none, an empty scissor box nothing drawn and none)
    (`ogles2-m8-1-20261025-184649`: 25, 93, 58 of 58; the GLES2 drawing
    tests as before, `ogles2-m7-4-20261025-184711`); dEQP-GLES3's
    `fence_sync` 10 of 10 and `occlusion_query` 62 of 62
    (`ogles2-m8-2-20261025-184450`, `-184530`). **Two traps:** a counter
    must be a group's first words: the hardware's groups are 16
    consecutive words on a 1024-byte boundary (Mesa's `V3D_OQ_GROUP_SIZE`),
    and an address past a group's 64 bytes is written elsewhere (each
    query's counter is the first word on the first 1 KB boundary of a 2 KB
    block); and the first draw of each pass sets the counter with its
    per-pass state (`draw.c`, `draw_state_configured`), which turned it off
    (now the active query's, `ctx->oq_addr`).
    **`glDrawBuffers` and `glReadBuffer` (item 11's, 2026-10-25):** ES
    3.0's `glDrawBuffers` is `EXT_draw_buffers`' entry; `glReadBuffer` sets
    a framebuffer object's read attachment (`read_buf`, which
    `glReadPixels` and the copies read: `read_attachment`) or the default
    framebuffer's `GL_BACK` or `GL_NONE` (`default_read_none`: reading
    then is `GL_INVALID_OPERATION`), with `GL_READ_BUFFER`. `m8_es3`'s
    errors and queries.
    **Primitive restart done (2026-10-24, on the A1200):**
    `GL_PRIMITIVE_RESTART_FIXED_INDEX` in ES3 contexts (`GL_INVALID_ENUM` in
    GLES2 ones; `OGLES2_CAP_PRIMITIVE_RESTART`); an indexed draw with it
    scans the indices in a loop of its own that skips the restart index
    (the branchless loops of other draws unchanged), writes it as the
    largest index of the type the GPU gets (also when `gl_VertexID` widens
    the indices to 32 bits) and sets the packets' restart bit (bit 31 of
    `INDEXED_PRIM_LIST`'s length). `m8_draw`'s strip ended by 0xFFFF and
    `m8_es3`'s state checks (`ogles2-m8-1-20261024-224815`: 15, 45, 18 of
    18 in each of the three drawing steps); the GLES2 drawing tests as
    before (`ogles2-m7-4-20261024-224826`). With it the vertex attribute
    part of `glu::resetState` passes; the next one, the rasterization
    state, stops at `GL_RASTERIZER_DISCARD` (stage C's;
    `ogles2-m8-2-20261024-224918`, 174 of 174 of `shaders-switch` with
    the patch).
    **Rasterizer discard done (2026-10-24, on the A1200):**
    `GL_RASTERIZER_DISCARD` in ES3 contexts (`OGLES2_CAP_RASTERIZER_DISCARD`):
    `glClear` and the draws do nothing while it is on (without transform
    feedback, stage C, a draw then has no effect; `draw_arrays` returns
    first; the copies' draws clear the caps). `m8_draw`'s clear and draw
    under it; with it the rasterization and multisampling parts of
    `glu::resetState` pass, and the texture part stops at the 2D array and
    3D textures (`ogles2-m8-2-20261024-232639`).
13. **Tests:** `m8_es3` grows per feature (package `m8-1`); dEQP-GLES3
    runs past its first case only once `glu::resetState`
    (`framework/opengl/gluStateReset.cpp`, after every case) raises no GL
    error in an ES 3.0 context (stage A item 6, 2026-10-23): the enables
    `PRIMITIVE_RESTART_FIXED_INDEX` and `RASTERIZER_DISCARD`; the buffer
    targets `COPY_READ`, `COPY_WRITE`, `PIXEL_PACK`, `PIXEL_UNPACK`,
    `UNIFORM` (with `glBindBufferBase` up to `MAX_UNIFORM_BUFFER_BINDINGS`)
    and `TRANSFORM_FEEDBACK`; `glBindVertexArray(0)`, `glBindSampler`,
    `glBindTransformFeedback`, `glDrawBuffers`, `glReadBuffer`; ES 3.0's
    pixel store (`PACK_`/`UNPACK_ROW_LENGTH`, `_SKIP_*`, `UNPACK_IMAGE_HEIGHT`,
    `_SKIP_IMAGES`); `TEXTURE_3D` and `TEXTURE_2D_ARRAY` bindings and the new
    texture parameters (`BASE_LEVEL`, `MAX_LEVEL`, `MIN_LOD`, `MAX_LOD`,
    `COMPARE_MODE`, `COMPARE_FUNC`, the four swizzles, `WRAP_R`);
    `FRAGMENT_SHADER_DERIVATIVE_HINT`; `GL_CURRENT_QUERY` and
    `TRANSFORM_FEEDBACK_ACTIVE` queries. The state alone, before each
    feature works, is the first step that lets the groups below run
    (**done 2026-10-25**: no error in the reset, patch `gles3/0001` gone;
    §1's hand-off lists the parts and the groups run without it);
    then dEQP-GLES3
    groups as their features land (`functional.vertex_array_objects`,
    `vertex_arrays`, `default_vertex_attrib`, `attribute_location`,
    `instanced`, `draw`, `buffer`, `ubo`, `samplers`, `texture` without its
    449 `texture.specification.*pbo*` cases, `fragment_out` (its 940 cases
    make their renderbuffers with `glRenderbufferStorageMultisample` and 0
    samples, `es3fFragmentOutputTests.cpp:281-282`, so that entry comes in
    stage B: 0 samples as `glRenderbufferStorage`, more refused until stage
    C), `fbo` without `fbo.msaa`, the 13 `fbo.completeness.samples` and the
    8 `fbo.invalidate.*msaa*` cases (those need stage C's pixel buffers and
    multisampled renderbuffers), `fence_sync`, `occlusion_query`,
    `primitive_restart`, `read_pixels`, `lifetime` without its 8
    `*transform_feedback*` cases; and the groups that need nothing of stage
    C, about 27,000 cases, so they test the shared code before 2.0 ships:
    `shaders` without its 120 `derivate.*msaa*` cases, `fragment_ops`
    (with 1,152 `blend.fbo_srgb`, sRGB targets that 2.0 offers as
    `EXT_sRGB`), `uniform_api`, `clipping`, `rasterization` without its 24
    `fbo.rbo_multisample_*`, `shader_api` without its 14 `program_binary`,
    `implementation_limits`, `polygon_offset`, `dither`, `color_clear`,
    `depth_stencil_clear`, `flush_finish`, `default_vertex_array_object`),
    from their own package
    (`m8-2`, Appendix B.5); in GLES2 contexts, dEQP-GLES2's cases the
    extensions change (`fbo.completeness.renderable`, `fbo.render`,
    `state_query`) and the whole dEQP-GLES2 list again, gl4es (`m6-1`) and
    Quake III through gl4es (`m6-2`).

Done when: these dEQP-GLES3 groups pass on the A1200, GLES2 and the speed
figures unchanged; **release 2.0** with the GLES2 extensions (§1).

## 6. Stage C: ES 3.0 complete (2-3 weeks)

1. **Transform feedback:** `glTransformFeedbackVaryings` before the link (the
   varyings and buffer mode go with `V3DGLSL_LINK_ES3`; Mesa makes them the
   binning shader's used outputs, and that shader writes the buffers; its
   transform feedback specs go into the file, §3), `glBeginTransformFeedback`
   and friends, the specs and buffers in the control list, a flush after
   each draw that feeds back (Mesa: without it the GPU may hang), pause and
   resume, the primitives-written query, `GL_RASTERIZER_DISCARD`, and a
   buffer written by transform feedback and read later waiting for it
   (Appendix B.3 has the packets and Mesa's sequence).
   - **ES 3.0's draw rules** while feedback is active and not paused: indexed
     draws are `GL_INVALID_OPERATION` (Mesa, `main/draw_validate.c:510-540`;
     dEQP-GLES3 `es3fNegativeVertexArrayApiTests.cpp:430-462`); the draw's
     primitive mode must be the one `glBeginTransformFeedback` named; a draw
     that would write past the end of a buffer is `GL_INVALID_OPERATION`.
     So feedback never meets primitive restart or index rebasing, and the
     CPU knows the primitives written exactly (the count of the mode's
     primitives times the instances): the query and the next draw's buffer
     offsets come from that count, without Mesa's stall to read the GPU's
     counts back. (Mesa's per-draw offset leaves the instance count out,
     `v3dx_draw.c:1398-1402`, and no dEQP-GLES3 feedback case is instanced:
     the library counts the instances.)
   - **Rasterizer discard** also discards clears (Mesa skips them:
     `glClear` at `main/clear.c:182-183`, `glClearBuffer*` at `:372`, `:392`,
     `:485`, `:585`, `:619` and `:717-718`; dEQP-GLES3's
     `rasterizer_discard.*.clear_*`): the library's clears in the tile list
     (`ogles2_job_clear`) are skipped while it is on.
2. **Multisampled framebuffer objects:** `glRenderbufferStorageMultisample`
   with samples (stage B's entry, which took 0 only; 4x: storage of twice the
   width and height, the job in 4x mode with the tile size it needs;
   `GL_MAX_SAMPLES` 4; a request for 1 to 4 samples gets 4), resolve by
   `glBlitFramebuffer`: the tile buffer's 4x decimation for whole,
   unscissored, equal boxes of internal types 8 and 16F (Appendix B.3), a
   shader resolve otherwise (ES 3.0 blits honour the scissor: dEQP-GLES3's
   `fragment_ops.scissor.framebuffer_blit_*`). `glGetInternalformativ` gives
   the sample counts per format: 4 for every colour-renderable format but the
   integer and 32-bit float ones, and for every depth and stencil format
   (dEQP-GLES3's `state_query.internal_format` requires `MAX_SAMPLES` of each,
   `es3fInternalFormatQueryTests.cpp:103-117`, `:183-217`), none for the
   32-bit float ones (dEQP-GLES3's minimums allow 0 for RGBA32F, RG32F, R32F,
   RGBA16F and RGB16F, and require 4 for R11F_G11F_B10F, RG16F and R16F,
   `es3fFboTestCase.cpp:281-338`), and integer formats refuse samples in ES
   3.0 (`GL_INVALID_OPERATION`, ES 3.0.0's rule, which Mesa applies in 3.0
   contexts, `main/multisample.c:237-249`).
3. **Pixel buffer objects:** `GL_PIXEL_PACK_BUFFER`, `GL_PIXEL_UNPACK_BUFFER`
   for `glReadPixels` and texture uploads. They use the buffer's CPU copy:
   the library converts pixels on the CPU anyway (stage B item 1 keeps the
   copies in step).
4. **The rest:** `glGetProgramBinary`/`glProgramBinary` (ES3 names),
   `glProgramParameteri`, everything left of Appendix A's ES 3.0 table.
5. **dEQP-GLES3's must-pass lists** on the A1200: `gles3-main` (with
   `transform_feedback`, `rasterizer_discard`, `pbo`, `multisample`,
   `negative_api`, `state_query` and what stage B left out) and
   `gles3-multisample` (§3).
6. **SDL:** the SDL2 and SDL3 ports pass ES 3 requests to ogles2 with the
   tag's 30 or 31 (today `OS3_GLES2_Wanted` accepts major version 2 only,
   Appendix B.5); a request for 3.1 on 2.1 is refused by the library and
   the port reports it, and the ports need the 2.1 SDK.

Done when: dEQP-GLES3 passes except documented deviations, as dEQP-GLES2
does; **release 2.1** with ES 3.0 contexts.

## 7. Stage D: ES 3.1 (5-7 weeks)

1. **Compute shaders.** The largest single piece:
   - `v3dcore`: a CSD submission path beside the binning/render one, under
     the same GPU lock (`v3d_gpu_lock.c`): the registers and the start, wait
     and timeout sequence of the kernel's `v3d_csd_job_run`, modelled on
     `v3d_tfu.c` (Appendix B.4), and the cache clean after it; each
     dispatch is waited for under the lock, as the TFU's `run` is
     (`v3d_tfu.c:107-136`), so compute jobs run one after another as in
     Mesa: `CSD_STATUS` bits 1:0 (current and queued dispatch) clear before
     the configuration is written, then the wait until the completed-jobs
     count (8 bits, wrapping) changes, then the clean;
   - the compiler: compute programs in the file (`v3dp_writer.c` accepts
     only vertex and fragment variants now), compiled by a dispatch in the
     fake device (Appendix B.1), and compute variants by key (the compute
     key carries `sampler_is_32b` from the compute stage's textures,
     `v3d_update_compiled_cs`), computed per dispatch as stage A computes
     them per draw;
   - the reader: the compute recipe types (`QUNIFORM_NUM_WORK_GROUPS`,
     `_WORK_GROUP_SIZE`, `QUNIFORM_SHARED_OFFSET`, `_SHARED_SIZE`; GL uses no
     `_WORK_GROUP_BASE`);
   - the library: `glDispatchCompute` (the CSD's configuration words as
     Mesa's `v3d_launch_grid` computes them; nothing dispatched for a zero
     count; a dispatch whose batch count would not fit `CFG4`'s 32 bits,
     over 2^32 batches, is split along z: the shader reads its work group
     id from the dispatch's payload (`nir_to_vir.c:3832-3844`), and whether
     the configuration words' offset fields reach it is unverified; if
     not, compute shaders are compiled with a work group base, Mesa's
     `load_base_workgroup_id` (`QUNIFORM_WORK_GROUP_BASE`, `:3847-3856`, as
     Mesa's Vulkan driver does for its dispatch base), and each part
     writes its base; Mesa's GL driver never splits),
     `glDispatchComputeIndirect` (the CPU reads the counts from the buffer
     bound to `GL_DISPATCH_INDIRECT_BUFFER` once the jobs writing them are
     done, as `v3d_launch_grid` does);
     shared memory per dispatch: the shader's size times the work groups
     that can be in memory at once, computed in 64 bits (Appendix B.4: the
     compiler masks the work-group-in-memory index, so at most 2^(16 -
     local invocation index bits) groups for groups of more than one
     invocation; Mesa allocates size times all work groups in 32 bits);
     `GL_OUT_OF_MEMORY` when the block cannot be had; a dispatch first
     flushes and waits for the jobs that write what it reads (a texture
     rendered to, a buffer fed back) and those that read what it writes, as
     Mesa's `v3d_predraw_check_stage_inputs` does (`drv/v3dx_draw.c:134-200`).
2. **Shader storage buffers, image load/store, atomic counters** (Mesa
   lowers atomic counters to storage buffers: an atomic counter buffer is
   the stage's storage slot `num_ssbos + binding`,
   `st_atom_atomicbuf.c:77-91`, recorded in the file, §3): the recipe types
   for their addresses and sizes (Mesa's `QUNIFORM_SSBO_OFFSET`,
   `_GET_SSBO_SIZE`, `QUNIFORM_IMAGE_*`), the buffers' GPU copies (stage B
   item 1, which also covers CPU writes into a buffer the GPU wrote), the
   `GL_SHADER_STORAGE_BUFFER` and `GL_ATOMIC_COUNTER_BUFFER` targets and
   their indexed bindings, `glBindImageTexture`, `glGetBooleani_v`, the
   image formats (all of ES 3.1's), and `OES_shader_image_atomic` (Mesa
   offers it in every ES 3.1 context, `main/extensions_table.h:488`; it
   gates dEQP-GLES31's image atomic cases,
   `es31fShaderImageLoadStoreTests.cpp:2193-2195`). An image binding gets a
   texture state record of its own: the unit's format, its one level and
   its layers (Mesa's `v3dx_state.c:1372-1394`; the 264
   `image_load_store.*.format_reinterpret` cases bind an image in another
   format than its texture's), not the texture's cached ones.
   - **The cache clean** after a job whose shaders write storage buffers,
     images or atomic counters (the file says which; prog data's
     `tmu_dirty_rcl` is also set by register spills,
     `vir_register_allocate.c:682`, and GLES2 programs that spill must not
     pay for cleans), before any later job may read the memory: such a
     render is not left running (`may_defer`), or the next submitter runs
     the clean under the GPU lock before it submits (`take_gpu`,
     `ogles2_job_finish`); the kernel cleans before it signals completion
     (`v3d_gem.c`).
   - **`glMemoryBarrier` and `glMemoryBarrierByRegion`** end the open job
     when it has such writes (then the clean), whatever the bits: within one
     job all binning comes before any rendering, so a later draw in the same
     job cannot see an earlier draw's stores. Mesa acts on the shader
     buffer and image bits only (`drv/v3d_context.c:94-114`) because it
     tracks each job's writes (`v3dx_draw.c:1200-1220`), which ogles2 does
     not; the CPU-side bits are covered by stage B item 1's waits too.
   - **Image stores mark the texture's RAM copy stale** per level and layer,
     as rendering into a texture does now (`texture.c`'s `rendered`, set
     from `fbo.c:1166-1180`), so renaming, `glGenerateMipmap` and partial
     uploads never use stale texels.
   - **No MMU:** the V3D's MMU is unused (`v3d_regs.h:191-194`), so a store
     outside its buffer or image lands in AmigaOS memory. Programs with
     storage blocks, atomic counters or images get `robust_storage_access`
     and `robust_image_access` in their keys (`v3d_compiler.h:373-376`):
     Mesa's compiler then keeps such accesses inside their buffers and
     images (`should_lower_robustness`, `vir.c:1871-1893`). Mesa sets them
     only for a robust context (`drv/v3d_program.c:594-595`;
     `robust_image_access` never), so a Mesa patch (as 0014) sets both for
     such programs, the library's key computation the same (GLES2 keys
     unchanged), and adds shared memory's intrinsics to that filter (NIR's
     `nir_lower_robust_access` has them; V3D's filter leaves them out). A
     binding with no buffer or image has size 0 at a valid dummy address,
     so the checks drop its stores. What it costs is measured. Local arrays
     indexed with a variable may live in scratch memory, whose stores have
     no bounds check (`nir_lower_vars_to_scratch`, `vir.c:2043`): a risk
     (§8).
3. **Indirect draws:** `glDrawArraysIndirect`, `glDrawElementsIndirect`.
   The library's draws need the vertex count and index range on the CPU:
   it converts and copies the attributes and indices per draw (§2), and its
   buffers' GPU copies are swapped per 32-bit word, which scrambles 16-bit
   indices and 8- and 16-bit attributes. So the library reads the command
   (`DrawArraysIndirectCommand`, 16 bytes; `DrawElementsIndirectCommand`,
   20) from the buffer on the CPU, once the jobs writing it are done (and
   `CachePostDMA`), and makes the direct draw it describes, base vertex and
   instance count included, as `glDispatchComputeIndirect` does (item 1).
   The hardware's indirect packets (Appendix B.4) would need the vertex and
   index data in the GPU's form across whole buffers; they are a later
   optimisation for draws whose attributes and indices are all 32-bit.
   ES 3.1's rules: `GL_INVALID_OPERATION` without a vertex array object
   other than 0, with a client-side array, without a buffer bound to
   `GL_DRAW_INDIRECT_BUFFER`, or with a command outside that buffer;
   `GL_INVALID_VALUE` for an offset that is not a multiple of 4
   (dEQP-GLES31's 13 `draw_indirect.negative` cases, `es31fDrawTests.cpp`).
4. **Textures and samples:** multisample textures
   (`glTexStorage2DMultisample`; `texelFetch` per sample is lowered by
   Mesa's compiler), `glSampleMaski` (the `SAMPLE_STATE` packet),
   `glGetMultisamplefv` (Mesa's 4x positions, Appendix B.4),
   `glGetTexLevelParameter*`, `textureGather`, stencil texturing
   (`GL_DEPTH_STENCIL_TEXTURE_MODE`), and `OES_texture_stencil8`
   (stencil-only textures; an ES 3.1 extension).
   Sample counts in ES 3.1 contexts: 4 for every renderable format but
   RGBA16F, R32F, RG32F and RGBA32F, integer ones included. ES 3.1 drops
   ES 3.0's refusal (`main/multisample.c:237-249`), and dEQP-GLES31's
   `state_query.internal_format` fails a format other than those four
   that offers none (`es31fInternalFormatQueryTests.cpp:113-117`,
   `:150-160`, `:205-216`); Mesa accepts 4 samples for any format
   (`v3d_screen.c:424`). So `GL_MAX_INTEGER_SAMPLES`,
   `_COLOR_TEXTURE_SAMPLES` and `_DEPTH_TEXTURE_SAMPLES` are 4, and
   integer multisampled renderbuffers and textures are built here (4x
   integer targets; a blit resolves them with the shader, taking one sample
   a pixel). The four float formats offer none: dEQP-GLES31's multisample
   texture cases report "not supported" for a count a format does not
   offer (`es31fTextureMultisampleTests.cpp:1235-1239`).
5. **Vertex attribute binding** (`glBindVertexBuffer`, `glVertexAttribFormat`,
   `glVertexAttribIFormat`, `glVertexAttribBinding`, `glVertexBindingDivisor`)
   and **separate shader objects** (program pipelines, `glProgramUniform*`).
   Separate shader objects are the awkward one: a `.v3dp` is a linked vertex
   and fragment pair, and the reader refuses anything else (`stages` must be
   0x11). A separable program may hold one stage or both, so format 2 allows a
   file of one stage and records a separable program's interface (§3), which
   `glGetProgramResource*` and pipeline validation need (dEQP-GLES31's 539
   `program_interface_query.program_{input,output}.*separable*` and 80
   `separate_shader.validation` cases). A pipeline links its stages at draw
   time: the interface is matched by location and by name
   (`es31fSeparateShaderTests.cpp:196-206`, `:352-355`), checked by
   `glValidateProgramPipeline` and at the draw, and its variants are compiled
   for the pair when first drawn (stage A's path: `V3DGLSL_COMPILE_VARIANT`
   then sends both programs' sources; the compiler links each with
   `GL_PROGRAM_SEPARABLE`, binds them with `glUseProgramStages`, draws once
   and compiles the key; a one-stage separable program has no link-time
   variants, since the key-state draws need both stages, and the offline
   compiler takes pairs), into a `.v3dx` named by the BLAKE3 of the two
   programs' hashes (vertex, then fragment; 24 hex digits, as a program's).
   Without a compiler a pipeline draws only from a shipped `.v3dx` for its
   pair, a deviation `ogles2.doc` lists.
6. **GLSL ES 3.10 and queries:** explicit uniform locations and
   `layout(binding)` (from the file, §3), arrays of arrays,
   `glGetProgramInterfaceiv`, `glGetProgramResource*`, everything left of
   Appendix A's ES 3.1 table. All of ES 3.1's state tables (dEQP-GLES31's
   `state_query`, 1,331 cases: limits, indexed bindings,
   `SAMPLE_MASK_VALUE`, `VERTEX_BINDING_*`, `PROGRAM_PIPELINE_BINDING`,
   `TEXTURE_BINDING_2D_MULTISAMPLE`, ...); some limits are tied to others
   (`es31fIntegerStateQueryTests.cpp:458-476`): `MAX_FRAMEBUFFER_WIDTH`
   and `_HEIGHT` at least `MAX_TEXTURE_SIZE` (4096),
   `MAX_FRAMEBUFFER_SAMPLES` at least `MAX_SAMPLES`. ES 3.1's errors
   (dEQP-GLES31's 496 `debug.negative_coverage.get_error` cases: compute
   dispatch limits and programs, indirect offsets and ranges, indexed
   query ranges, `glMemoryBarrierByRegion`'s bits, multisample textures,
   atomic counters, images, ...). **Framebuffers without attachments**
   (`glFramebufferParameteri`, `glGetFramebufferParameteriv`): the job takes
   its size and samples from the default parameters, has one render target
   and stores none (`STORE_TILE_BUFFER_GENERAL` with buffer NONE, as Mesa,
   `v3dx_rcl.c:314-321`); today such a framebuffer cannot be drawn
   (`fbo.c:1117`, `job.c:848-850`; dEQP-GLES31's 72 `fbo.no_attachments`
   cases).
7. **dEQP-GLES31's must-pass lists** on the A1200 (package `m8-3`),
   `gles31-main` and `gles31-multisample`; the ES 3.2 extensions' groups
   report "not supported" (§3).

Done when: dEQP-GLES31 passes except documented deviations, as GLES2 and
GLES3 do; `GL_VERSION` "OpenGL ES 3.1 ogles2.library 2.2"; **release 2.2**.

## 8. Risks

| Risk | Effect | Answer |
|---|---|---|
| The library's key computation differs from Mesa's | GLES2 draws miss their variants: compiles, or the closest variant, where 1.3 drew right | 1.x's matching kept for the fields it covers; computed keys checked against `v3dglslc`'s; Quake III from a fresh install compiles nothing (stage A) |
| Buffers with two copies (CPU and GPU) get out of step | wrong data in uniform buffers or after transform feedback | one rule for which copy is current, every CPU access synchronised (stage B item 1), tested by dEQP-GLES3's `buffer`, `ubo` and `transform_feedback` |
| GPU-written data used by the CPU or as vertices, indices or indirect commands | a full stall and a swap at every use (feedback ping-pong, draws built by compute) | correct first; the hardware's indirect packets and direct use of 32-bit GPU copies later (stage D item 3) |
| A shader store outside its buffer or image, with no MMU | AmigaOS memory overwritten | robust storage and image access, and shared memory, through a Mesa patch (stage D item 2); the V3D's MMU, unused today, would contain it (not planned) |
| A local array indexed out of range in scratch memory (any GLSL version) | AmigaOS memory overwritten | undefined behaviour in GLSL ES, as in 1.x; a Mesa patch clamping scratch offsets considered in stage D, its cost measured; `ogles2.doc` says so |
| Draw-time variants: a state change mid-game waits for the compiler | a stall of about 0.1 s, once per new state | the cache keeps them; programs can ship their `.v3dx` (stage A) |
| No compiler installed, or an old one, and a variant missing | the closest variant, or for some states no draw | logged with the program's hash; programs ship their variants; `ogles2.doc` says so |
| More variant keys: more compiled variants per program (software blending keys carry formats and blend state) | bigger cache files, longer first starts | variants only for states actually drawn, not precompiled for every state |
| GLSL ES 3.x compiles may be slower on the 68k | longer first starts | the offline compiler, shipped binaries, the cache, as with GLES2 |
| ES3 state checks slow GLES2 draws | released programs get slower | Quake III and glmark2 timed at each stage's end against 1.x (§3); ES3 checks only in ES3 contexts or where an ES3 feature is in use |
| `main` and `gles3` number the same thing | two entries, tags or request codes with one number | `main`'s numbers first; `gles3`'s renumbered at each merge until released; format 2 provisional until 2.1 (§1, §3) |
| ES3 programs' files reach a 1.x or older 2.x library | wrong drawing | format 2: 1.x refuses them at the header; 2.x readers refuse unknown feature bits (§3) |
| Cached files from an older compiler | a built-in limit compiled with an old value | the installer clears `OGLES2Cache:` when the compiler's output changes (§3) |
| Texture memory: 1.3 keeps a RAM copy of every level (`texture.c`'s `shadow`, for `glGenerateMipmap` and renaming) | 3D and array textures take twice their size, in RAM and GPU memory | measured in stage B; immutable textures (`glTexStorage*`) may drop the copy where nothing needs it |
| Library and compiler size, memory | a bigger `ogles2.library` (240,312 bytes at 1.3.1); a `v3dglslc` above today's 19 MB | measured per stage |
| A 2.x library with a 1.x compiler in `LIBS:` | no ES3 shaders | the info log says so; `Install-ogles2` replaces the compiler with the library (§3); GLES2 programs served as by 1.3 |
| GLES2 regressions from shared code (`job.c`, `draw.c`, `buffer.c`, `texture.c`, `fbo.c`, the reader) | breaks released programs | the GLES2 regression set at each stage's end; `main` stays releasable |
| dEQP-GLES3/31 on a big-endian CPU | false failures from dEQP itself | the GLES2 port needed three big-endian fixes in dEQP (patches 0001-0003: delibs' CPU, the ETC1 `.pkm` loader, the point-size check's pixel read); the GLES3 and GLES31 modules use more of the framework (texture formats, ETC2 and EAC references, buffer read-backs), so failures are checked for that cause before the library is blamed, and fixes join `ports/deqp/patches` |
| dEQP-GLES3/31 runs: 6 hours and more, large result logs | slow progress; `amictld` stalls on large replies | fix the stall before stage B's runs; runs split by group |
| Compute and storage writes: cache coherency | wrong results, rarely | the kernel's clean sequence and Mesa's waits followed exactly, also after deferred renders (Appendix B.4, stage D item 2); dEQP-GLES31's `synchronization`, `ssbo`, `image_load_store` |
| A compute job that hangs | the GPU stops for every program | the kernel's rule: a job is stuck only while its batch counter stops changing; then the GPU reset `ogles2_device_reset` already does (a whole-GPU reset, which covers the CSD) |
| ES3 work needs Mesa patches | the 1.x compiler built from a patched tree | a Mesa tree of its own for this work from stage A (§3) |
| `main` and `gles3` drift apart | painful merges | merge `main` into `gles3` after each `main` release, and before each stage ends |

## 9. Order and checkpoints

| Stage | Contents | Estimate | Checkpoint |
|---|---|---|---|
| (1.4) | GLES2 contexts refuse GLSL ES 3.x; the `OGLES2_VERSION` and hash-order texts (§1) | days | on `main` |
| A | ES3 contexts (test builds), format 2, ES3 link state, flat/centroid, `gl_VertexID`, variants by key | 3-4 weeks | `m8_es3` (`m8-1`); dEQP-GLES2, the regression set and speed unchanged |
| B | GPU buffer copies, buffers in full, VAOs, instancing, vertex formats, UBOs, samplers, textures, ETC2, render targets and blending, framebuffers, blit, sync, queries | 5-6 weeks | the dEQP-GLES3 groups; **release 2.0** (GLES2 extensions) |
| C | transform feedback, MSAA FBOs, PBOs, the rest | 2-3 weeks | dEQP-GLES3 complete; **release 2.1** (ES 3.0 contexts) |
| D | compute, SSBOs, images, atomics, indirect, MS textures, sample mask, gather, attribute binding, SSO, framebuffers without attachments | 5-7 weeks | dEQP-GLES31 complete; **release 2.2** (ES 3.1) |

15-20 weeks, about 3.5-4.5 months, at the pace of M7's extensions; the
dEQP-GLES3/31 runs on the A1200 add their own hours per stage. Before stage
B's first dEQP runs: `amictld`'s large-reply stall (§3).

## 10. Open decisions

- The context tag's name and values (`OGLES2_CCT_PISTORM_ES_VERSION`, 20,
  30, 31, "this version or later", §3), provisionally
  `OGLES2_CCT_PISTORM_MIN + 6`, the next free one; fixed by 2.0, which
  knows it.
- The release split (§1): 2.0 after B with GLES2 extensions, ES3 contexts in
  2.1 after C, ES 3.1 in 2.2 (recommended: stage A and B's changes to
  shared code reach users while they are still small to bisect, gl4es gains
  `EXT_texture_rg`, `OES_depth24` and `EXT_frag_depth`, and no half ES 3.0
  is ever offered; format 2 is not frozen before 2.1); or one release after
  C, which is then 2.0, and what this plan says of 2.0 and 2.1 applies to
  it.
- `EXT_occlusion_query_boolean` and `EXT_shadow_samplers` in GLES2 contexts:
  stage B builds both features, but Quake III's GLES2 renderer looks for
  both (§1) and would take other paths with them; not in 2.0
  (recommended), later with Quake III timed.
- `EXT_sRGB_write_control` in ES3 contexts from 2.1 (SDL3's GLES2 renderer
  looks for it, `src/render/opengles2/SDL_render_gles2.c:2442`; turning
  sRGB writes off is the render target's format, RGBA8 instead of
  SRGB8_ALPHA8): recommended, small.
- Geometry shaders (`GL_EXT_geometry_shader`) after stage D, or not at all
  (recommended: only if a program asks; dEQP-GLES31 has 207 cases in
  `geometry_shading` and about 1,100 more that name geometry shaders in
  other groups: 741 in `shaders`, 175 in `program_interface_query`, 100 in
  `primitive_bounding_box`, 55 in `state_query`, 31 in
  `tessellation_geometry_interaction`).

## Appendix A: the ES 3.0 and 3.1 commands by stage

From `gl.xml`'s `GL_ES_VERSION_3_0` (104) and `GL_ES_VERSION_3_1` (68)
features; each command once.

**ES 3.0** (stage A 18, B 71, C 14, and one already in 1.3):

| Stage | Group | Commands |
|---|---|---|
| A | queries (2) | `glGetStringi`, `glGetInteger64v` |
| A | unsigned and non-square uniforms (15) | `glUniform1ui`, `glUniform2ui`, `glUniform3ui`, `glUniform4ui`, `glUniform1uiv`, `glUniform2uiv`, `glUniform3uiv`, `glUniform4uiv`, `glGetUniformuiv`, `glUniformMatrix2x3fv`, `glUniformMatrix3x2fv`, `glUniformMatrix2x4fv`, `glUniformMatrix4x2fv`, `glUniformMatrix3x4fv`, `glUniformMatrix4x3fv` |
| A | fragment outputs (1) | `glGetFragDataLocation` |
| B | vertex array objects (4) | `glBindVertexArray`, `glDeleteVertexArrays`, `glGenVertexArrays`, `glIsVertexArray` |
| B | instancing (3) | `glDrawArraysInstanced`, `glDrawElementsInstanced`, `glVertexAttribDivisor` |
| B | integer attributes (7) | `glVertexAttribIPointer`, `glVertexAttribI4i`, `glVertexAttribI4ui`, `glVertexAttribI4iv`, `glVertexAttribI4uiv`, `glGetVertexAttribIiv`, `glGetVertexAttribIuiv` |
| B | uniform buffers (10) | `glBindBufferRange`, `glBindBufferBase`, `glGetIntegeri_v`, `glGetInteger64i_v`, `glGetUniformIndices`, `glGetActiveUniformsiv`, `glGetUniformBlockIndex`, `glGetActiveUniformBlockiv`, `glGetActiveUniformBlockName`, `glUniformBlockBinding` |
| B | sampler objects (10) | `glGenSamplers`, `glDeleteSamplers`, `glIsSampler`, `glBindSampler`, `glSamplerParameteri`, `glSamplerParameteriv`, `glSamplerParameterf`, `glSamplerParameterfv`, `glGetSamplerParameteriv`, `glGetSamplerParameterfv` |
| B | textures (7) | `glTexImage3D`, `glTexSubImage3D`, `glCopyTexSubImage3D`, `glCompressedTexImage3D`, `glCompressedTexSubImage3D`, `glTexStorage2D`, `glTexStorage3D` |
| B | render targets and framebuffers (10) | `glClearBufferiv`, `glClearBufferuiv`, `glClearBufferfv`, `glClearBufferfi`, `glDrawBuffers`, `glReadBuffer`, `glBlitFramebuffer`, `glInvalidateFramebuffer`, `glInvalidateSubFramebuffer`, `glFramebufferTextureLayer` |
| B | sync objects (6) | `glFenceSync`, `glIsSync`, `glDeleteSync`, `glClientWaitSync`, `glWaitSync`, `glGetSynciv` |
| B | occlusion queries (7) | `glGenQueries`, `glDeleteQueries`, `glIsQuery`, `glBeginQuery`, `glEndQuery`, `glGetQueryiv`, `glGetQueryObjectuiv` |
| B | buffers (6) | `glMapBufferRange`, `glFlushMappedBufferRange`, `glUnmapBuffer`, `glGetBufferPointerv`, `glCopyBufferSubData`, `glGetBufferParameteri64v` |
| B | renderbuffer storage (1; samples from stage C) | `glRenderbufferStorageMultisample` |
| 1.3 | already in the table (AmigaOS 4's entry) (1) | `glDrawRangeElements` |
| C | transform feedback (10) | `glBeginTransformFeedback`, `glEndTransformFeedback`, `glTransformFeedbackVaryings`, `glGetTransformFeedbackVarying`, `glBindTransformFeedback`, `glDeleteTransformFeedbacks`, `glGenTransformFeedbacks`, `glIsTransformFeedback`, `glPauseTransformFeedback`, `glResumeTransformFeedback` |
| C | internal format queries (1) | `glGetInternalformativ` |
| C | program binaries (3) | `glGetProgramBinary`, `glProgramBinary`, `glProgramParameteri` |

**ES 3.1** (all stage D):

| Stage | Group | Commands |
|---|---|---|
| D | compute (2) | `glDispatchCompute`, `glDispatchComputeIndirect` |
| D | storage, images, barriers (4) | `glBindImageTexture`, `glGetBooleani_v`, `glMemoryBarrier`, `glMemoryBarrierByRegion` |
| D | indirect draws (2) | `glDrawArraysIndirect`, `glDrawElementsIndirect` |
| D | multisample textures, sample mask, level queries (5) | `glTexStorage2DMultisample`, `glGetMultisamplefv`, `glSampleMaski`, `glGetTexLevelParameteriv`, `glGetTexLevelParameterfv` |
| D | vertex attribute binding (5) | `glBindVertexBuffer`, `glVertexAttribFormat`, `glVertexAttribIFormat`, `glVertexAttribBinding`, `glVertexBindingDivisor` |
| D | separate shader objects (43) | `glUseProgramStages`, `glActiveShaderProgram`, `glCreateShaderProgramv`, `glBindProgramPipeline`, `glDeleteProgramPipelines`, `glGenProgramPipelines`, `glIsProgramPipeline`, `glValidateProgramPipeline`, `glGetProgramPipelineiv`, `glGetProgramPipelineInfoLog`, `glProgramUniform1i`, `glProgramUniform2i`, `glProgramUniform3i`, `glProgramUniform4i`, `glProgramUniform1ui`, `glProgramUniform2ui`, `glProgramUniform3ui`, `glProgramUniform4ui`, `glProgramUniform1f`, `glProgramUniform2f`, `glProgramUniform3f`, `glProgramUniform4f`, `glProgramUniform1iv`, `glProgramUniform2iv`, `glProgramUniform3iv`, `glProgramUniform4iv`, `glProgramUniform1uiv`, `glProgramUniform2uiv`, `glProgramUniform3uiv`, `glProgramUniform4uiv`, `glProgramUniform1fv`, `glProgramUniform2fv`, `glProgramUniform3fv`, `glProgramUniform4fv`, `glProgramUniformMatrix2fv`, `glProgramUniformMatrix3fv`, `glProgramUniformMatrix4fv`, `glProgramUniformMatrix2x3fv`, `glProgramUniformMatrix3x2fv`, `glProgramUniformMatrix2x4fv`, `glProgramUniformMatrix4x2fv`, `glProgramUniformMatrix3x4fv`, `glProgramUniformMatrix4x3fv` |
| D | program interface queries (5) | `glGetProgramInterfaceiv`, `glGetProgramResourceIndex`, `glGetProgramResourceName`, `glGetProgramResourceiv`, `glGetProgramResourceLocation` |
| D | framebuffers without attachments (2) | `glFramebufferParameteri`, `glGetFramebufferParameteriv` |

2.0's GLES2 extensions add 18 suffixed entries: `OES_vertex_array_object`
(4), `EXT_instanced_arrays` (3), `OES_texture_3D` (6), `EXT_texture_storage`
(`glTexStorage2DEXT`, and `glTexStorage3DEXT` with `OES_texture_3D`: ES 2.0
has no 1D textures, and the `glTextureStorage*EXT` forms need
`EXT_direct_state_access`), `EXT_map_buffer_range` (2), and
`OES_draw_elements_base_vertex`'s `glDrawElementsInstancedBaseVertexOES`
(with instancing: stage B). ES3 contexts add its
`glDrawRangeElementsBaseVertexOES` (built in stage B, so already in 2.0's
table, where it refuses in GLES2 contexts; usable with 2.1's ES3 contexts,
§3). `OES_depth24` and `EXT_frag_depth` add no commands.

## Appendix B: research for the implementation

Gathered 2026-10-18 and 19. Paths: `mesa/` is `~/mesa-build/mesa/` (the pinned
commit with patches 0001-0014), `drv/` is `mesa/src/gallium/drivers/v3d/`,
`cle` is `mesa/src/broadcom/cle/v3d_packet.xml` (V3D 4.2 is version 42 in
it), `kernel` is Linux's `drivers/gpu/drm/v3d/`, `cts` is `~/vk-gl-cts/`.
Line numbers are those of these trees on that day; the names are what to
search for.

### B.1 The compiler (`tools/v3dglslc`, Mesa's front end and V3D driver)

- **How variants are captured now.** Patch 0009 adds
  `v3d_compiled_variant_hook()` (stage, key, key size, prog data, QPU code
  and its size; `drv/v3d_program.c:466-470`), called after each `v3d_compile`
  while it is set; it also bypasses the driver's in-memory and disk caches.
  The writer's `variant_hook` (`v3dp/v3dp_writer.c:259-312`) keeps up to 64
  variants, drops byte-identical ones, and accepts only vertex
  (`sizeof(struct v3d_vs_key)`, kind 0, or 1 when `is_coord`) and fragment
  (kind 4) shaders:
  geometry and compute set `capture_failed`. Patch 0014 forces key fields
  with a global (`v3d_force_alpha_to_coverage`, read in
  `v3d_update_compiled_fs`); `serve()` maps request codes to write modes at
  `v3dglslc.c:500`. A global flag and a real draw is the only way to steer a
  key today.
- **Compiling an exact key.** `v3d_get_compiled_shader()` (context, key, key
  size, uncompiled shader) is declared `static` (`drv/v3d_program.c:42-45`;
  the definition at `:472-476` lacks the word but keeps that linkage), so the
  patch adds an exported wrapper. Its cache hashes the key bytes (BLAKE3 with
  the uncompiled shader's hash) and compares them with `memcmp`: a key built
  from the file must be zeroed first, padding included, then filled field by
  field. Key sizes: `struct v3d_vs_key`, `struct v3d_fs_key`, and the plain
  `struct v3d_key` for compute. The uncompiled shaders are
  `v3d_context(st->pipe)->prog.bind_vs`, `bind_fs` and `bind_compute` after
  one draw or dispatch (gallium's V3D has no threaded-context wrapper, so
  `st->pipe` is the V3D context). A variant depends only on the key, the
  uncompiled shader's NIR and the screen's compiler. (`v3d_compile()` is
  exported, `mesa/src/broadcom/compiler/v3d_compiler.h:1111`, but would bypass
  the driver's uniform and prog-data handling.)
- **Key fields and their sources** (`v3d_setup_shared_key`,
  `v3d_update_compiled_fs` `drv/v3d_program.c:636-776`, `_vs` `:891-998`):
  - common: `sampler_is_32b` bit *i* when the bound view's format returns
    32 bits (`v3d_get_tex_return_size`); `robust_uniform_access` and
    `robust_storage_access` from the context's robust-buffer flag
    (`drv/v3d_program.c:594-595`), `robust_image_access` never (all 0
    today; stage D's patch sets the storage and image ones, stage D item
    2); `null_descriptor` 0;
  - fragment: `is_points`/`is_lines` from the primitive and polygon mode;
    `line_smoothing` (lines, smoothing on, no MSAA); `msaa`,
    `sample_alpha_to_coverage`, `sample_alpha_to_one` from the rasterizer and
    blend state when the job is multisampled (any attachment with more than
    one sample), else `ignore_sample_mask` 1; `swap_color_rb` always 0 at this
    commit; `can_earlyz_with_discard` (discard, no depth or stencil writes, no
    occlusion query); `software_blend`, which is
    `framebuffer_soft_blend || blend->use_software` (`drv/v3d_program.c:693`),
    where `framebuffer_soft_blend` is set when any bound colour buffer's
    format cannot be blended in the tile buffer (`v3d_fb_needs_soft_blend`,
    `drv/v3dx_state.c:506-536`), whether blending is on or not; `cbufs` one
    bit per bound buffer; `color_fmt[i]` format and swizzle only with a logic
    op or software blending, `blend[i]` only with software blending
    (`drv/v3d_program.c:695-738`); `logicop_func` the blend state's logic op
    or `PIPE_LOGICOP_COPY` (12; 0 is CLEAR, and `v3d_nir_lower_logic_ops.c`
    would make the shader write zeros, `:61-62`; COPY skips the pass,
    `:385-386`); `f32_color_rb` for 32-bit float channel 0, 16-bit normalized,
    signed normalized and pure integer formats (not RGBA16F); `norm_16`,
    `snorm`; `point_sprite_mask`, `point_coord_upper_left` (points);
    `ucp_enables`; `has_gs`;
  - vertex: `used_outputs`/`num_used_outputs` from the fragment variant's
    `input_slots`/`num_inputs` (so the library takes them from the fragment
    variant it chose); `per_vertex_point_size`; `va_swap_rb_mask` for BGRA
    and BGR10A2 vertex formats; `clamp_color` 0; the binning variant
    (`is_coord`) outputs only the transform feedback varyings (none without
    transform feedback), after X, Y, Z, W, Xs, Ys and the point size;
  - compute: the plain key, from the compute stage's textures
    (`v3d_update_compiled_cs`, `drv/v3d_program.c:1008-1029`).
- **Interface information after the link**
  (`mesa/src/mesa/main/shader_types.h`): - uniform blocks:
  `prog->data->UniformBlocks[NumUniformBlocks]` (`gl_uniform_block`: `name`,
  `Uniforms[]`, `Binding`, `UniformBufferSize`, `stageref`, `_Packing`,
  `_RowMajor`); member offsets and strides in `UniformStorage[]` entries with
  `block_index >= 0` (`offset`, `array_stride`, `matrix_stride`, `row_major`;
  for storage blocks also `top_level_array_size` and `_stride`). **Per
  stage:** the shader reads block *i* of its own list,
  `prog->sh.UniformBlocks[i]`, as `QUNIFORM_UBO_ADDR` unit *i* + 1
  (`mesa/src/broadcom/compiler/nir_to_vir.c:568-577`,
  `mesa/src/mesa/state_tracker/st_atom_constbuf.c:301-328`), and the program's
  list appends each stage's blocks in stage order
  (`gl_nir_link_uniform_blocks.c:263-340`), so a stage's order is not the
  program's: the file records the map per stage (§3); - storage blocks:
  `ShaderStorageBlocks[]`, the same structure, and the same per-stage map;
  each stage's `info.num_ssbos`; - atomic counter buffers:
  `data->AtomicBuffers[]` (`Binding`, `MinimumSize`, `Uniforms`,
  `StageReferences`), `UniformStorage::atomic_buffer_index`; lowered to
  storage buffers at slot `num_ssbos + Binding` (`st_glsl_to_nir.cpp:352-364`,
  `st_atom_atomicbuf.c:77-91`); - fragment outputs: `ProgramResourceList[]`
  entries of type `GL_PROGRAM_OUTPUT` (`gl_shader_variable`: `location`
  relative to `FRAG_RESULT_DATA0`, `index`, `component`); a separable
  program's inputs and outputs are `GL_PROGRAM_INPUT`/`GL_PROGRAM_OUTPUT`
  entries the same way; - transform feedback:
  `prog->last_vert_prog->sh.LinkedTransformFeedback` (`Outputs[]`:
  `OutputRegister`, `OutputBuffer`, `NumComponents`, `DstOffset`,
  `ComponentOffset`; `Varyings[]`: `name`, `Type`, `BufferIndex`, `Size`,
  `Offset`; `Buffers[4]`: `Stride` in words); the linker's input is
  `prog->TransformFeedback` (`BufferMode`, `VaryingNames`, `NumVarying`),
  which the request must set before the link; - explicit uniform locations:
  `prog->UniformRemapTable` (a `struct range_remap`, looked up with
  `util_range_remap`), `NumExplicitUniformLocations`,
  `gl_uniform_storage::remap_location`; - `layout(binding)`: samplers and
  images in the uniform storage's initial value
  (`gl_nir_link_uniform_initializers.c:60-61`), blocks in
  `gl_uniform_block::Binding`, atomics in `gl_active_atomic_buffer::Binding`.
  **Trap:** `v3dglslc`'s `bind_samplers` sets every sampler uniform and resets
  them afterwards (`v3dglslc.c:265-293`, called at `:309` and `:352`) before
  the file is written: the initial bindings must be read before the key-state
  draws; - **the key-state draws:** `bind_samplers` binds textures only for
  `sampler2D`, `samplerCube` and `sampler2DRect` (`:275`), and
  `draw_key_states` (`:299`) draws every program into one RGBA8 target
  (`make_target`), and one writing `gl_FragData[1]` or up also into 2, 3 and 4
  RGBA8 targets (`:327-346`). Programs need a texture of the right kind for
  every sampler type: `sampler3D` already in 2.0's GLES2 contexts
  (`OES_texture_3D`'s directive compiles in GLSL ES 1.00; tested 2026-10-19,
  and the compiler then wrote no `.v3dw`, since it counts only the three kinds
  as samplers), then 2D array, shadow, integer and unsigned ones (an integer
  texture for an `isampler`, so `sampler_is_32b` comes out as real draws give
  it); and ES3 programs need targets matching the fragment outputs' types (an
  integer output into an integer target): a variant drawn into RGBA8 for an
  integer output is one no real draw uses; - compute: the shader's
  `info.workgroup_size[3]`, `shared_size`; prog data
  `struct v3d_compute_prog_data` (`local_size[3]`, `shared_size`,
  `can_use_supergroups`) besides the common `threads`, `single_seg`,
  `has_control_barrier`, `spill_size`.
- **Transform feedback specs.** The state tracker turns the linked info into
  `pipe_stream_output_info` (`get_stream_output_info_from_nir`,
  `mesa/src/mesa/state_tracker/st_program.c:773-806`); the driver's
  `v3d_set_transform_feedback_outputs` (`drv/v3d_program.c:79-182`) packs
  16-bit `TRANSFORM_FEEDBACK_OUTPUT_DATA_SPEC`s (at most 16, each at most 16
  values; first value at VPM offset 6, `tf_specs_psiz` one later for a
  written point size) into the uncompiled shader: `num_tf_specs`,
  `tf_specs[16]`, `tf_specs_psiz[16]`, `stream_output.stride[4]`. The
  compiler reads them from `prog.bind_vs` after a draw (it includes
  `v3d_context.h`): no Mesa patch for this.
- **Compute variants:** `v3d_update_compiled_cs` (`drv/v3d_program.c:1008`)
  compiles with the plain key on `launch_grid`; the fake device accepts
  `SUBMIT_CSD`, so a `glDispatchCompute(1, 1, 1)` in the compiler triggers
  the compile (unverified end to end).
- **GLSL limits.** The parser copies `ctx->Const` at every compile, so
  `v3dglslc` may set it after creating the context (it does,
  `v3dglslc.c:604-610`). Built-ins and their sources: `gl_MaxVertexAttribs`
  (`Program[VS].MaxAttribs`, 16), `gl_MaxVertexOutputVectors` and
  `gl_MaxFragmentInputVectors` (`Program[].MaxOutputComponents/4`,
  `MaxInputComponents/4`: 16), `gl_Max*TextureImageUnits` (set to 16 a stage
  and 32 combined; 48 combined from 2.0, §2), `gl_MaxDrawBuffers` (4),
  `gl_Min/MaxProgramTexelOffset` (-8, 7), `gl_MaxImageUnits`,
  `gl_Max*ImageUniforms`, `gl_MaxCombinedShaderOutputResources`,
  `gl_Max*AtomicCounters` and `*AtomicCounterBuffers`,
  `gl_MaxAtomicCounterBindings`, `gl_MaxAtomicCounterBufferSize`,
  `gl_MaxComputeWorkGroupCount` (65535) and `Size` (256),
  `gl_MaxComputeUniformComponents`, `gl_MaxComputeTextureImageUnits` (set to
  16, as the other stages). Checked by the compiler and linker, not built-ins:
  `MaxComputeWorkGroupInvocations` (256), `MaxComputeSharedMemorySize`
  (32768), `Program[].MaxShaderStorageBlocks` (fragment and compute share
  their slots with atomic counter buffers; vertex 0),
  `MaxCombinedShaderStorageBlocks`, `MaxShaderStorageBufferBindings`,
  `Program[].MaxUniformBlocks`, `MaxCombinedUniformBlocks`,
  `MaxUniformBufferBindings` (at least 36 for ES 3.1, §2),
  `MaxUniformBlockSize`, `MaxTransformFeedbackBuffers` (4),
  `...InterleavedComponents` (64), `...SeparateComponents` (4),
  `MaxUserAssignableUniformLocations` (at least 1024 for ES 3.1), `MaxVarying`
  (16). Sources: `mesa/src/mesa/state_tracker/st_extensions.c`. Stage A prints
  them all from the compiler once, and the library's queries are set from that
  list.

### B.2 The library: where each change lands

- **Context and queries:** `known_tag` (`context.c:22-45`, unknown tags
  logged at `:84-87`), `ogles2_glGetString` (`:297-327`),
  `ogles2_unimplemented` (`:332`); limits in `state.c`'s `lookup`
  (`:546-790`, at most 8 values, `struct value` at `:544`), then
  `ogles2_texture_query` (`texture.c:679-702`), `ogles2_buffer_query`,
  `ogles2_fbo_query` (`fbo.c:1337`); clear and blend colours clamped at
  `state.c:157-160`, `:435`; defines in `ogles2_internal.h`
  (`OGLES2_MAX_VERTEX_ATTRIBS` 16, `_TEXTURE_UNITS` 32, `_STAGE_SAMPLERS` 16,
  `_DRAW_BUFFERS` 4, `_IMAGE_ATTRIBS` 64); `struct OGLES2Context`
  (`ogles2_internal.h:442-533`; `share_lock` must stay at offset 4);
  `struct OGLES2Share` (`:61-76`). `OGLES2_CCT_PISTORM_MIN + 6` is free
  (`include/libraries/ogles2.h:68`).
- **Draws** (`draw.c`): `ogles2_glDrawArrays` (`:1710`), `draw_elements`
  (`:1743`), the core `draw_arrays` (`:1445-1663`: variant choice
  `:1464-1535`, uniform upload `write_uniforms` `:493-567`, `ubo_copies`
  `:362` (the default block's little-endian copy for `QUNIFORM_UBO_ADDR`
  unit 0), `spill_area` `:389`, `texture_states` `:462`), the attribute
  record `attr_record` (`:817-826`: divisor bytes always 0, "read as
  int/uint" never set), `copy_array` (`:744`, float, fixed, short, byte),
  `constant_attrib` (`:794-808`), `default_attribs` (`:608-624`),
  `shader_record` (`:831-960`), `emit_state` (`:1090-1324`: one
  `BLEND_ENABLES 0x0F` and `BLEND_CFG` at `:1266-1272`; the "zero all"
  flat, non-perspective and centroid packets, transform feedback specs off
  and occlusion counter 0 at `:1305-1310`), `draw_packets` (`:1342`:
  `VERTEX_ARRAY_PRIMS` from vertex 0 or `INDEX_BUFFER_SETUP` +
  `INDEXED_PRIM_LIST`, restart off); the indices are scanned for their range
  (`:1795-1837`) and copied rebased to the lowest one, 16- and 32-bit
  swapped (`:1601-1625`). The command writers exist only for some packets
  (`v3dcore/include/v3d_commands.h`); the opcodes of the instanced,
  indirect, base vertex, transform feedback and occlusion packets are in
  `v3d_hw.h`.
- **Jobs** (`job.c`): `ogles2_job_begin` (`:175-258`, the tile size at
  `:210-219`, from the framebuffer object's target; `TileBinningModeCfg` with
  its bpp, `:238`), `ogles2_job_clear` (`:268-297`, the colour packed to 8
  bits, and kept as floats for a 16F target), `build_render` for the default
  framebuffer (`:300-439`) and `build_render_fbo` (`:514-659`: per target
  32-bit type 8 or 64-bit 16F, `TileRenderingModeCFGColorEach` at `:627`; no
  MSAA; `clear_colors`, parts 1 and 2, `:482-501`), `may_defer` and `take_gpu`
  (`:749-797`), `ogles2_job_flush` (`:808-1008`; a framebuffer object without
  GPU memory drops the job at `:848-850`), the depth/stencil save across
  flushes `zs_save` (`:115-161`).
- **Textures and framebuffers:** `struct OGLES2Texture` (`texture.c:58-92`),
  `OGLES2TexState` (`:99-110`, 2D, cube and rectangle units only), the copy
  classes (`:716-720`), `complete` (`:1152-1170`), `storage_of`
  (`:1188-1246`), `pack`/`pack_wide`, `ogles2_texture_state` (`:2741-2897`:
  the record cache at `:2765-2771`, no wrap R, compare fixed to NEVER, 32-bit
  float and depth textures forced to nearest at `:2846`, `unfilterable_type`),
  renaming `before_change` (`:231-274`), the `rendered` mark of a level drawn
  into (`:53`, set from `fbo.c:1166-1180`), the TFU path `store_tfu`
  (`:1517-1543`), `ogles2_glCompressedTexImage2D` (`:2436-2484`, ETC1 decoded
  at `:2472`), `tex_image` (2D, rectangle and cube only, `:2072`),
  `ogles2_texture_renderable` (`:1718-1734`: RGBA8, 565, 4444, 5551, and
  half-float RGBA), `ogles2_texture_half` (`:1736`), `read_half` (`:1841`),
  the scratch texture for copies from the default framebuffer
  (`ogles2_texture_scratch`, `:1930`); `copy.c` (`glCopyTex*` on the GPU),
  `ogles2_draw_copy` (`draw.c:1951`), the context's own framebuffer object for
  copies (`ogles2_fbo_copy_target`, `fbo.c:312`); `fbo.c`:
  `struct OGLES2Renderbuffer`/`Attachment`/`Framebuffer` (`:69-100`),
  renderbuffer formats (`:166-190`), `status_of` (`:878-919`),
  `ogles2_fb_update` (`:938`), the pools (`pools`, `:1035-1077`),
  `ogles2_fbo_target` (`:1079`; the widest target and the tile size at
  `:1097-1116`, size 0 refused at `:1117`), the attachment queries
  (`:1288-1334`; component type at `:1247`).
- **Buffers** (`buffer.c`): `struct OGLES2Buffer` (`:56-66`, `data` in the
  68k's byte order, `copy[8]`), `glDeleteBuffers` (`:199-238`), the target and
  usage checks (`bound`, `:275-283`; `:256`, `:293`, `:297`, `:390`, `:426`,
  `:461`, `:479`, `:500`), `buffer_data` (`:285-335`), `patch_copies`
  (`:351-381`), `glMapBufferOES` (`:455`, write-only, the CPU copy),
  `ogles2_buffer_copy` (`:576-634`).
- **Programs:** `struct OGLES2Program`, `OGLES2Image`, `OGLES2Variant`
  (`ogles2_internal.h:181-364`; a variant keeps only `per_vertex_point_size`,
  `is_points`, `is_lines`, `cbufs` of its key); `program_hash`
  (`program.c:677-700`: vertex, fragment, then "BIND"); `load_image`
  (`:913-955`, the lookup that stops at a refused file at `:926`),
  `extra_image` (`:987-1028`), `bind_image` (uniform locations,
  `program.c:1088-1121`), `glLinkProgram` (`:1134-1192`, the sources kept at
  `:1172-1173`), program binaries (`:1361-1438`), `set_uniform` and
  `type_shape` (`:1580-1684`), `glUniformMatrix*fv` (`:1789-1817`),
  `glCompileShader` (`:402-454`, no `#version` check); `compiler.c`:
  `compiler_where` (`:28`), the server start (60 s), `request` (`:208-265`, no
  time limit; a running server is used at `:237`), the request functions
  (`:267-308`); `v3dp.c`: `ogles2_v3dp_load` (`:160-533`; major 1 only at
  `:179`, the validated Mesa commits at `:21`, 64 sections at `:169` and
  `:193`, matrix columns at `:344-345`), `recipe_ok` (`:138-145`),
  `ogles2_image_variant` (`:565-592`).
- **GPU** (`v3dcore/hw/`): the lock (`v3d_gpu_lock.c`), binning and render
  submission with bounded waits (`v3d_submit_timeout.c`; the queue registers
  in `v3d_hw.c:89-118`; 8-bit frame counters), `v3d_invalidate_caches`
  (`v3d_hw.c:16-36`, before binning and rendering, `job.c:884`, `:915`), the
  TFU's `run` (`v3d_tfu.c:107-136`), memory from `AllocVec` with bus address
  = CPU address, aligned to 64 bytes (`v3d_mem_allocvec.c:45`, `:81-98`; no
  MMU: its registers are defined but unused, `v3d_regs.h:191-194`),
  `ogles2_device_reset` (`device.c:243`, a whole-GPU reset). **Register
  byte order:** most registers are written as natural values through
  `LE32` (the TFU's, `v3d_tfu.c:118-125`, `v3d_regs.h:75-96`): the CSD's
  follow that; `L2TCACTL` is written raw with pre-swapped constants
  (`V3D_L2TCACTL_L2TFLS` 0x01000000, `v3d_regs.h:54-61`, `v3d_hw.c:20`): the
  cache clean follows that, so TMUWCF (bit 8) is raw 0x00010000 and
  `L2TFLS | FLM_CLEAN` (0x5) raw 0x05000000, and reads of it are compared
  the same way (derived from the existing constant; checked on the A1200
  in stage D). **CPU caches:** `CachePreDMA` or `CacheClearE` after the CPU
  writes what the GPU reads (`job.c:95`, `:682`, `draw.c:219`, `:788`,
  `texture.c:1528`), and `CachePostDMA` before the CPU reads what the GPU
  wrote (`display.c:503`, `:524`, `texture.c:1541`). **Job serials:**
  `OGLES2LibBase->serial`, handed out per job (`job.c:248-250`), unique
  across contexts.
- **Generator:** `gen/gen_ogles2.py` (`read_gl`, `resolve_order`
  `:141-164`, which exits when `abi_order.txt` lacks an entry unless
  `--append` is given, `implemented` by a regex over `ogles2/*.c`, stubs
  calling `ogles2_unimplemented`, `client/procaddr.c`: thunks and a sorted
  name table in the client), `gen/verify_table.py`; the client's
  `OGLES2Open` (`client/ogles2_open.c:43-70`: `OpenLibrary` with the
  compiled-in `OGLES2_VERSION`, the ABI and size checks at `:60`).
- **Release:** `build_release.sh` (`VERSION` `:42`; the SDK from `include/`
  and the generated headers), `Install-ogles2` (the library `:20-33`, the
  compiler `:38-53`), `ogles2.doc` (EXTENSIONS `:73`, LIMITS `:113`,
  deviations `:183`, `OGLES2Open` and `OGLES2_VERSION` `:210-246`,
  `aglCreateContext2` `:265`), `OGLES2_LIB_VERSION` and `_REVISION`
  (`ogles2_internal.h:21-23`).

### B.3 The GPU for ES 3.0 (Mesa's V3D driver, version 42)

- **Draw packets** (`cle` 562-647): `INDEXED_PRIM_LIST` (32),
  `INDEXED_INSTANCED_PRIM_LIST` (34: number of instances, instance length,
  restart enable, index type 0/1/2 for 8/16/32 bits), `VERTEX_ARRAY_PRIMS`
  (36), `VERTEX_ARRAY_INSTANCED_PRIMS` (38), the indirect ones (33, 37),
  `BASE_VERTEX_BASE_INSTANCE` (43, for the next draw only), `INDEX_BUFFER_SETUP`
  (44). Mesa uses the instanced packets when the instance count is above 1
  (`drv/v3dx_draw.c:1322`, `:1354`). The instance count is 32 bits.
- **Attribute record** (`GL_SHADER_STATE_ATTRIBUTE_RECORD`, `cle` 1473, 16
  bytes): vec size (0 = 4), type (half float 1, float 2, fixed 3, byte 4,
  short 5, int 6, int 2_10_10_10 7), signed, normalized, read as int/uint,
  values read by the coordinate and the vertex shader, instance divisor (16
  bits; Mesa clamps to 0xffff), stride, maximum index (32 bits; Mesa
  writes 0xffffff). Mesa fills it in `v3d_vertex_state_create`
  (`drv/v3dx_state.c:358-429`). Rules: at least one attribute read by both
  shaders, a dummy attribute when none is bound (GFXH-930,
  `v3dx_draw.c:849-866`). Integer attributes: the driver's
  `is_format_supported` lists no pure integer vertex formats
  (`drv/v3d_screen.c:431-489`), yet `v3d_vertex_state_create` sets "read as
  int/uint" from the format (`v3dx_state.c:387`) and gives integer
  attributes an integer default w of 1 (`:440-456`), and Vulkan's V3D
  driver offers them (`mesa/src/broadcom/vulkan/v3dv_formats.c`): the
  record does them. Missing from the list as well, and converted by
  `u_vbuf`: unsigned 32-bit values read as float (R32 UNORM/USCALED) and
  three-component half floats (`u_vbuf.c:212-277`, `:294-306`).
- **Primitive restart:** the indexed packets' restart bit; fixed index only
  (Mesa draws without restart for other indices, `v3dx_draw.c:1139-1146`).
- **Varying flags:** `FLAT_SHADE_FLAGS` (98), `NON_PERSPECTIVE_FLAGS` (100),
  `CENTROID_FLAGS` (89), each 24 varyings at an offset with actions for the
  lower and higher ones, and their "zero all" packets (97, 99, 88); Mesa's
  `emit_varying_flags` writes them from prog data's flag words
  (`drv/v3dx_emit.c:126-208`, `:621-643`).
- **Transform feedback:** `TRANSFORM_FEEDBACK_SPECS` (74, enable and count,
  then the 16-bit specs), `TRANSFORM_FEEDBACK_BUFFER` (73: address, size in
  words, number), `FLUSH_TRANSFORM_FEEDBACK_DATA` (75),
  `WAIT_FOR_TRANSFORM_FEEDBACK` (14), `TRANSFORM_FEEDBACK_FLUSH_AND_COUNT`
  (31, after every draw that feeds back: without it the GPU may hang,
  `v3dx_draw.c:1382-1386`), `PRIMITIVE_COUNTS_FEEDBACK` (72, at the end of
  the binning list, with the specs disabled after it, `v3dx_job.c:34-71`).
  The buffer address is the binding's offset plus the vertices written so
  far times the stride (`v3dx_emit.c:680-711`). Pause stores the counts and
  continues from them (`v3d_set_stream_output_targets`, `v3dx_state.c:1272`);
  Mesa's primitives written come from the stored counts
  (`v3d_job.c:523-550`). Rasterizer discard: both facing enables off in
  `CFG_BITS` and no rendering for the job. A buffer read in the same job
  after it was written: flush and wait packets
  (`v3d_emit_wait_for_tf_if_needed`, `v3dx_draw.c:290-336`).
- **Occlusion queries:** `OCCLUSION_QUERY_COUNTER` (92, an address; 0 turns
  it off); the counter is zeroed by the CPU at the start and read after the
  job; both "any samples" targets answer "non-zero"; Mesa lays counters out
  in groups of 16 consecutive 32-bit words, each group aligned to 1024
  bytes (its comment, `drv/v3d_query_pipe.c:38-44`).
- **Render targets** (`get_internal_type_bpp_for_output_format`,
  `drv/v3dx_format_table.c:221-342`): 8-bit formats 32 bpp (type 8, or 8I
  and 8UI for the integer ones); sRGB, RGB10_A2, R11G11B10F and the 16-bit
  float ones type 16F, 64 bpp, RG16F and R16F included ("so the TLB doesn't
  throw away the alpha"; sRGB is converted at load and store); RGBA16I,
  RGBA16UI and RGB10_A2UI 64 bpp, the RG and R 16-bit integer ones 32;
  32-bit ones 128, 64, 32 bpp by channel count; 16-bit normalized formats
  are emulated as 16-bit integer ones. Clamp: normalized for sRGB, integer
  for integer formats, none otherwise. The render configuration
  (`drv/v3dx_rcl.c:719-923`): `TILE_RENDERING_MODE_CFG_COMMON` (maximum bpp,
  render-target count, 4x mode, depth type), per target the clear colour in
  `CLEAR_COLORS_PART1`, `PART2` from 64 bpp, `PART3` at 128 bpp or for a
  UIF pad (`:854`), then
  `TILE_RENDERING_MODE_CFG_COLOR` (each target's bpp, type and clamp), the
  depth/stencil clear last. Clear colours are clamped and packed per type
  on the CPU (`v3d_tlb_clear`, `v3dx_draw.c:1663-1771`). The tile size
  (`v3d_choose_tile_size`, `mesa/src/broadcom/common/v3d_util.c:122-206`)
  steps down from 64x64 with more targets, MSAA and the largest bpp
  (`:185-199`).
- **Blending** (`drv/v3dx_emit.c:95-125`, `:339-341`, `:525-569`): the tile
  buffer blends only internal types 8 and 16F
  (`v3d_format_supports_tlb_resolve_and_blend`, `drv/v3d_formats.c:148`);
  one bound target of another type makes `framebuffer_soft_blend`, which
  switches the tile buffer's blending off for all targets (`CFG_BITS`'
  blend enable 0, `BLEND_ENABLES` with mask 0, no `BLEND_CFG`) and has the
  shader blend
  (the key's `software_blend`; integer targets are skipped by the shader's
  blend lowering). With tile-buffer blending, `BLEND_ENABLES` carries the
  per-target mask and `BLEND_CFG` is sent per target where they differ,
  `blend_dst_alpha_one` making DST_ALPHA a factor of ONE for formats
  without alpha (`v3dx_state.c:539-551`, `v3dx_emit.c:539`). Mesa's state
  tracker turns blending off for integer draw buffers
  (`st_atom_blend.c:178-183`, `:289`).
- **Texture state** (`TEXTURE_SHADER_STATE`, `cle` 1592-1631): base and max
  level, swizzle (0 zero, 1 one, 2-5 R-A), texture type, width, height and
  depth (14 bits each), array stride (64-byte units), sRGB bit; set in
  `drv/v3dx_state.c:895-1029` (3D: depth; arrays: layer count). Sampler
  (`SAMPLER_STATE`, `cle` 1709): wrap S, T and R, compare function, min and
  max LOD, anisotropy, border mode. The TMU parameters (`v3d_tex.c`): P0
  holds the texture state address and the returned words, P1 the sampler
  address and the 32-bit output flag (from the format's return size), P2
  the operation, offsets, gather mode and component. Formats
  (`v3dx_format_table.c:61-209`): ETC2/EAC as in stage B item 9, all 16-bit
  return; `RGB9_E5` texture only; `R11F_G11F_B10F` renderable; 8-bit
  signed normalized textures, not renderable; depth `DEPTH24_X8`,
  `DEPTH_COMP32F`, `DEPTH_COMP16` (32-bit return); integer texture types
  96-114 (8 and 16-bit ones return 16 bits, 32-bit ones 32).
- **Blits** (`drv/v3d_blit.c`): `v3d_tlb_blit_fast` (colour, a job still
  open that wrote the whole source: an extra store), `v3d_tfu_blit` (whole
  surface, same format, no scaling), `v3d_tlb_blit` (tile-aligned boxes;
  colour, depth, stencil; MSAA either side), `v3d_stencil_blit`,
  `v3d_render_blit` (a textured quad). The tile-buffer paths need the same
  render-target format (different formats only if neither is emulated in
  the shader), no scissor, equal boxes (`check_tlb_blit_ok`, `:345-399`); a
  resolve needs internal type 8 or 16F (the same condition as blending).
  MSAA resolve: `STORE_TILE_BUFFER_GENERAL` with 4x decimation
  (`v3dx_rcl.c:157-168`).
- **MSAA objects:** 4x mode in both binning and rendering configuration,
  double-buffer off, smaller tiles; multisampled storage is one level at
  twice the width and height (`drv/v3d_resource.c:616-648`), stored with
  all samples; `rasterizer_oversample_mode` and `SAMPLE_STATE` (91) for the
  sample mask (`v3dx_emit.c:324-334`, `:730-738`). Mesa accepts 4 samples
  for any format it supports (`v3d_screen.c:424`); ES 3.0 contexts refuse
  integer formats with samples (`main/multisample.c:237-249`).

### B.4 The GPU for ES 3.1

- **CSD registers** (`kernel/v3d_regs.h:407-478`, core registers, V3D 4.x
  offsets): `V3D_CSD_STATUS` 0x900 (completed jobs in bits 11:4, active 3:2,
  current and queued dispatch bits 1 and 0), `V3D_CSD_QUEUED_CFG0`-`6`
  0x904-0x91c, `V3D_CSD_CURRENT_CFG0`-`6` 0x920-0x938, `V3D_CSD_CURRENT_ID0`/`1`
  0x93c/0x940; the interrupt bit `V3D_INT_CSDDONE` is bit 7 on 4.x. ogles2's
  `v3d_regs.h` has none of them yet; they are written through `LE32`
  (B.2).
- **Configuration words** (`mesa/src/broadcom/common/v3d_csd.h:27-41`,
  filled in `v3d_launch_grid`, `drv/v3dx_draw.c:1486-1563`): CFG0-2 the work
  group count of x, y, z in bits 31:16 (offsets in 15:0, 0 in GL); CFG3
  work groups per supergroup (bits 11:8, 0 = 16), batches per supergroup
  minus one (19:12) and the work group size (7:0, 256 written as 0); CFG4
  the number of batches minus one; CFG5 the shader address with the
  propagate-NaNs (bit 2, V3D before 7.1), single-segment (bit 1) and
  threading (bit 0, set when the shader runs 4 threads,
  `v3dx_draw.c:1535-1540`) flags; CFG6 the uniforms address. The supergroup
  choice is `v3d_csd_choose_workgroups_per_supergroup` (`v3d_util.c:33-99`);
  batches per supergroup = round-up(work groups per supergroup x size /
  16).
- **Start and wait** (`kernel/v3d_sched.c`, `v3d_csd_job_run`): nothing is
  dispatched when a count is 0; invalidate the caches (as ogles2 does
  before binning); write CFG1-CFG6, then CFG0, which starts the job;
  one dispatch can be queued behind the current one (`CSD_STATUS` bits 0
  and 1), and ogles2 writes only when both are clear; completion is the
  completed-jobs counter (8 bits, wrapping) changing (Mesa's simulator polls
  it, `mesa/src/broadcom/simulator/v3dx_simulator.c:208-249`). Timeout: the
  kernel treats a job as hung only when `CURRENT_CFG4` (batches left) has
  not changed since the last check (`v3d_csd_job_timedout`).
- **Cache clean** (`kernel/v3d_gem.c`, `v3d_clean_caches`), after a CSD job
  and after a binning/render job whose shaders wrote memory, before the job
  counts as done: wait until `L2TCACTL.L2TFLS` is clear (GFXH-1897); write
  `L2TCACTL = TMUWCF` (bit 8) and wait for it to clear; write
  `L2TFLS | FLM_CLEAN<<1` (FLM bits 2:1, clean = 2) and wait for `L2TFLS` to
  clear (raw values in B.2). ogles2 does only the invalidation
  (`v3d_invalidate_caches`: L2T flush and the slice caches). Mesa marks such
  jobs by prog data's `tmu_dirty_rcl` (set for any TMU write, spills included)
  and makes a later job reading the written buffers wait
  (`drv/v3d_job.c:206-213`, `v3dx_draw.c:1192-1198`); compute jobs run one
  after another. `glMemoryBarrier`: Mesa submits the jobs with TMU writes for
  the shader buffer and image bits (`drv/v3d_context.c:94-114`).
- **Indirect:** `glDispatchComputeIndirect` maps the 12 bytes on the CPU
  after the writers finish (`v3dx_draw.c:1462-1479`); the plan reads
  indirect draw commands the same way (stage D item 3). For the later
  optimisation, the hardware's indirect draws: the packets (33: mode, index
  type, record count, restart, address, stride in 4-byte units; 37: mode,
  count, address, stride) read GL's `DrawArraysIndirectCommand` and
  `DrawElementsIndirectCommand` as they are (Mesa passes the GL buffer
  straight to them); `INDEX_BUFFER_SETUP` points at the whole index buffer,
  so `firstIndex` counts from its start; the binning waits for the jobs that
  wrote the buffer, and double buffering is off (`v3dx_draw.c:1154-1198`,
  `:1305-1353`, `:1406`).
- **Storage and images in shaders**
  (`mesa/src/broadcom/compiler/nir_to_vir.c:502-725`): TMU general reads and
  writes (`TMUD` data, then the address to `TMUAU` with a configuration
  word: operation, per-pixel bit, type), atomic operations as TMU operations
  (add, sub, xchg, cmpxchg, min/max, and, or, xor; `cle` 87-100), `TMUWT`
  after stores, the barrier as a TMU flush then the `SYNCB` barrier.
  Uniform values (`drv/v3d_uniforms.c`): `QUNIFORM_SSBO_OFFSET` the
  buffer's address plus its offset, `_GET_SSBO_SIZE` its size,
  `QUNIFORM_IMAGE_TMU_CONFIG_P0` the image's texture state address (unit in
  the data word's top byte), `QUNIFORM_IMAGE_WIDTH`/`HEIGHT`/`DEPTH` the
  level's size, `_ARRAY_SIZE` the layers, `QUNIFORM_NUM_WORK_GROUPS` and
  `_WORK_GROUP_SIZE` the dispatch's, `QUNIFORM_SHARED_OFFSET` the shared
  memory's address, `_SHARED_SIZE` the size per work group. Robust access:
  the key's `robust_storage_access` and `robust_image_access`
  (`v3d_compiler.h:373-376`).
- **Shared memory:** the shader addresses
  `SHARED_OFFSET + wg_in_mem x shared_size`, where `wg_in_mem` comes from
  the payload and, for work
  groups of more than one invocation, is masked to 16 - local invocation
  index bits (those are `ffs(npot(max(size, 64))) - 1`, 6 to 8;
  `nir_to_vir.c:4905-4925`): so the block needs
  `shared_size x min(work groups, 2^(16 - bits))` bytes, at most 32 KB x
  1024 = 32 MB, and for
  one-invocation groups `shared_size x min(work groups, 65536)`. Mesa
  allocates `shared_size x num_wgs` in 32 bits (`v3dx_draw.c:1542-1550`),
  which can overflow; zero-initialising it assumes whole 4 KB pages
  (`vir.c:817-829`). Spills: `qpu_count x 4 x spill_size`
  (`drv/v3d_program.c:547-561`). Image formats: every ES 3.1 one
  (`drv/v3d_screen.c:525-540`); storage buffer offsets aligned to 4.
- **Multisample textures and samples:** each pixel a 2x2 quad (texture
  state with twice the width and height); `texelFetch` per sample is
  lowered to a plain fetch (`v3d_nir_lower_txf_ms.c`); `SAMPLE_STATE` (91):
  coverage and a 4-bit mask; sample positions for 4x
  (`drv/v3d_context.c:361-375`): (0.375, 0.125), (0.875, 0.375), (0.125,
  0.625), (0.625, 0.875).
- **Gather and stencil texturing:** gather mode and component in P2
  (`v3d_tex.c:258-263`; the result reordered by the compiler); stencil of a
  D24S8 texture read as `RGBA8UI` with swizzle XXXX
  (`v3dx_format_table.c:152`), of D32F_S8 from its separate R8UI stencil.
- **Framebuffers without attachments:** a store of buffer NONE in the
  render list (`v3dx_rcl.c:314-321`).

### B.5 Tests and ports

- **dEQP port** (`ports/deqp`): `build.sh` copies `framework`,
  `modules/glshared`, `modules/gles2` and `data/gles2` (`:65`), defines
  `DEQP_SUPPORT_GLES2` (`:99`), lists the framework sources (`:112-164`) and
  the module's (`:165-166`), links `deqp-gles2` (`:193`). GLES3 and GLES31
  are separate programs from `modules/gles3` (entry
  `tes3TestPackageEntry.cpp`; data `data/gles3` and
  `external/graphicsfuzz/data/gles3/graphicsfuzz`) and `modules/gles31`
  (`tes31TestPackageEntry.cpp`, `data/gles31`); `tgl45es3*` and `tgl45es31*`
  are desktop GL's and stay out (unverified whether they would build). The
  platform `src/tcuAmigaPlatform.cpp` refuses any API but ES 2.0 (`:101`),
  sets its tags (`:115-118`; it already accepts 4 samples, `:108-110`) and
  calls `glw::initES20` (`:127`): ES3 needs the version tag and
  `initES30`/`initES31`, which the CTS has. `caselists.sh` reads
  `gles2-main.txt` only (`:10`), `run_groups.py` runs package
  `ogles2-m7-1` only (`:20`): both are to take the module, the list and the
  package as arguments. (Done for ES 3.0 in stage A item 6, 2026-10-23:
  `MODULE=gles3` for `build.sh` and `caselists.sh`, `--package=` for
  `run_groups.py`, package `m8-2`; ES 3.1 adds `gles31` the same way.) The must-pass configurations
  (`cts/external/openglcts/data/gl_cts/data/mustpass/gles/aosp_mustpass/main/mustpass.xml`):
  `main` with `rgba8888d24s8ms0` and `multisample` with `rgba8888d24s8ms4`,
  256x256, on any OS.
- **Test packages:** a test is `tests/<mN_name>/<name>.c` with a Makefile
  rule and an entry in `all`; a package is `tests/a1200/<name>/` (`build.sh`,
  `Run-M1`, `ReadMe.txt`, `textfiles`, `notes-template.txt`), built by
  `tests/a1200/build_package.sh <name>`; `m8-1` (this work's own tests,
  `m8_es3`, from stage A) starts as a copy of `m7-4`, with its `libs`
  assigned first (§3); `m8-2` runs dEQP-GLES3 (stage B on) and `m8-3`
  dEQP-GLES31 (stage D), as `m7-1` runs dEQP-GLES2.
- **SDL ports:** `OS3_GLES2_Wanted` accepts only profile ES with major
  version 2 (`ports/sdl2/src/video/amigaos3/SDL_os3opengles2.c:52-57`, the
  same file in `ports/sdl3` `:53-56`), so an ES 3 request goes to MiniGL or
  fails; the context's tags are built in `OS3_GLES2_CreateContext` (SDL2
  `:168-173`, SDL3 `:175-180`). SDL3's GLES2 renderer looks for
  `GL_EXT_sRGB_write_control` (`src/render/opengles2/SDL_render_gles2.c:2442`
  in the port's sources).
- **gl4es** (`~/gl4es`, the port's pinned sources): the GLES extensions it
  probes are in `src/glx/hardext.c` (`OES_depth24` `:327`,
  `OES_texture_stencil8` `:335`, `EXT_texture_rg` `:338`, the float and
  half-float ones only with its `floattex` option `:339-343`,
  `EXT_frag_depth` `:377`); `src/gl/gles.h` only declares the others.
