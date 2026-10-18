# OpenGL ES 3.0 and 3.1 on ogles2.library

**Status: plan (2026-10-18), work on branch `gles3`.** Nothing of it is
implemented yet.

ogles2.library 1.3 is a complete OpenGL ES 2.0 with 32 extensions. The GPU it
drives, the Raspberry Pi 4's V3D 4.2, is OpenGL ES 3.1 capable: Mesa's V3D
driver, which `v3dglslc` is built from, is conformant for GLES 3.1 on it
under Linux. This plan extends ogles2 to OpenGL ES 3.0, then 3.1, as far as
the hardware allows, without changing anything for GLES2 programs.

Not in scope: tessellation (V3D 4.2 has no tessellation hardware) and the
rest of OpenGL ES 3.2. Geometry shaders exist on V3D 4.2 and in Mesa's
driver, so they could follow later as an extension (`GL_EXT_geometry_shader`).

## 1. Branches and releases

- **`main`** is the released 1.3 line. Fixes and small additions go there
  and are released as 1.4, 1.5 and so on (the library's revision; the
  dispatch table only grows, the ABI version stays).
- **`gles3`** is this work. It takes in `main` regularly (a merge from
  `main` after each `main` release at least), so fixes made for 1.x reach the
  ES3 code, and the GLES2 regression tests on `gles3` test today's 1.x code.
- **Release:** once stage B (below) is done and the GLES2 regression set
  passes unchanged, `gles3` is merged into `main` and released as
  **ogles2.library 2.0**: the same library, a GLES2 or GLES3 context by
  request. Later stages follow on `main` as 2.x, or stay on `gles3` until
  done; decided at that merge.
- Tags as now: `v1.4`, ..., `v2.0`. The release archives (`release/dist/`)
  are refreshed on `main` only.

## 2. What exists

- **GPU:** V3D 4.2: compute shader dispatch (the CSD unit), cache flush for
  shader writes, the texture formatting unit (TFU), instanced and indirect
  draws in the control list, occlusion counters, transform feedback,
  multisampled tiles, 16- and 32-bit float and integer render targets, sRGB,
  ETC2/EAC and BC1-BC3 textures in the TMU, `textureGather`.
- **Compiler:** `v3dglslc`'s fake DRM device (`tools/v3dglslc/compat/fake_drm.c`)
  already reports CSD, cache flush, TFU, perfmon and multisync, so Mesa's V3D
  driver enables compute shaders, storage buffers and images. Mesa's GLSL
  front end compiles GLSL ES 3.00 and 3.10. `v3dglslc` today creates one
  GLES 2.0 context (`attribs.major = 2`).
- **Library:** the GLES 2.0 core, 32 extensions, 164 dispatch entries. Already
  covering parts of ES 3.0: several render targets (`EXT_draw_buffers`),
  float, half-float and depth textures (sampled), 32-bit index buffers,
  program binaries, standard derivatives, S3TC, 32 texture units, 4x MSAA on
  the default framebuffer.
- **`.v3dp` format 1.0** (`v3dp/FORMAT.md`): stages vertex, binning and
  fragment; uniform recipes of Mesa's types 0-46, of which the reader
  implements a subset (`recipe_ok` in `ogles2/v3dp.c`: no uniform blocks
  other than the default one, no storage buffers, images or compute values).
- **Tests:** the dEQP-GLES2 port (`ports/deqp`), the A1200 test packages, the
  corpus, Quake III, glmark2. VK-GL-CTS, the source of the dEQP port, also has
  the `dEQP-GLES3` and `dEQP-GLES31` modules.

## 3. Shared ground

- **One library, two context versions.** A new context tag,
  `OGLES2_CCT_PISTORM_ES_VERSION` (2 by default, 3 for an ES 3.x context),
  chooses the API. A GLES2 context behaves exactly as in 1.x: the ES3
  entry points refuse with `GL_INVALID_OPERATION` there, `GL_VERSION` stays
  "OpenGL ES 2.0", and GLSL ES 3.x shaders are refused. An ES3 context
  reports "OpenGL ES 3.0" (later 3.1) and accepts `#version 100`, `300 es`
  and (stage D) `310 es`. AmigaOS 4 has no ogles3.library to be compatible
  with; one library keeps one driver and one SDK.
- **Entry points** come from Khronos' `gl.xml` as now: `feature
  GL_ES_VERSION_3_0` and later `GL_ES_VERSION_3_1` in `gen/api.txt`, each new
  command appended to `gen/abi_order.txt` when it is implemented (the table
  only grows). ES 3.0 adds 104 commands, ES 3.1 68 more. `<GLES3/gl3.h>` and
  `<GLES3/gl31.h>` from Khronos join `include/`.
- **Library version 2.0** for the first release with ES3 contexts (§1). The
  ABI version does not change: 1.x programs run on 2.0.
- **The compiler:** a second Mesa context in `v3dglslc` created as ES 3.1
  (`attribs.major = 3, minor = 1`); a request names the context version (a
  new field in `struct V3DGLSLRequest`, read only by a compiler that knows
  it: older senders' requests end before it). The offline compiler takes it
  as an option. Every compiler change rebuilds both compilers (host and
  m68k, byte-identical, `compare_v3dp.sh`) and invalidates the shipped
  caches once.
- **`.v3dp` 1.1** (a minor version: appended fields and sections only): the
  compute stage (Mesa's stage 5), the uniform recipe types the reader learns
  (stage A, D), uniform block and storage buffer bindings in the
  program's interface. The reader skips what it does not know, so a 1.0
  library refuses nothing it could run before.
- **Tests:** each stage brings its ogles2 tests and its dEQP groups
  (`ports/deqp` builds `dEQP-GLES3` from stage B and `dEQP-GLES31` from
  stage D, their must-pass lists as with GLES2). The GLES2 regression set
  runs unchanged at each stage's end: dEQP-GLES2 (17,165), `m5-1`, the corpus
  `m2-5`, `m6-6`, `m6-10`, Quake III `m6-5`, glmark2 `m6-7`, the examples.
- **Docs:** `ogles2.doc` gains an ES3 section and its deviations,
  `GettingStarted.txt` an ES3 chapter, the SDK the ES3 headers, the plan's
  Appendix B the new extensions.

## 4. Stage A: GLSL ES 3.00 and draw-time variants (2-3 weeks)

The base everything else stands on.

1. **ES3 contexts in the library:** the context tag, version strings and
   `GL_SHADING_LANGUAGE_VERSION`, the limits ES 3.0 raises (`GL_MAX_*`),
   `glGetIntegerv` and `glGetStringi`.
2. **GLSL ES 3.00 in `v3dglslc`:** the ES 3.1 Mesa context, the request
   field, the offline compiler's option; `in`/`out`, `layout(location)`,
   integer and unsigned types, `texelFetch`, `textureSize`, `textureLod`,
   switch, bit operations, array constructors.
3. **The reader's new recipe types and interface:** unsigned uniforms
   (`glUniform*ui`), integer samplers (the TMU's integer return), 3D, array
   and shadow samplers, uniform blocks (`QUNIFORM_UBO_ADDR` for units other
   than 0, block bindings), `gl_InstanceID` and `gl_VertexID`.
4. **Draw-time variant compilation, in general.** Today a program's
   variants are compiled for the default draw states at link time, plus two
   special cases (`.v3dw` for 32-bit samplers, `.v3da` for alpha to
   coverage). ES3 states that change the compiled code (integer and float
   render targets, sRGB, multisampled framebuffer objects, transform
   feedback, integer samplers) make that one general path:
   - the library computes Mesa's key for the draw, as it does now, and on a
     miss sends it to the compiler (`V3DGLSL_COMPILE_VARIANT`: the program's
     sources and the key);
   - the compiler draws with that state, captures the variant (patch 0009's
     hook), and appends it to the program's `.v3dp` in the cache (a section
     of its own; the file stays valid for older readers);
   - the draw waits for it once (about 0.1-0.5 s), later starts load it;
   - `.v3dw` and `.v3da` become two uses of the same path.
5. **Tests:** a new `m8_es3` (GLSL ES 3.00 features against expected pixels,
   a GLES2 context still refusing `#version 300 es`), the variant path
   (a program drawn in states with no precompiled variant, then from the
   cache without the compiler).

Done when: GLSL ES 3.00 programs compile, link and draw in an ES3 context;
dEQP-GLES2 is unchanged.

## 5. Stage B: the ES 3.0 core that saves CPU (3-4 weeks)

The 68k CPU is the limit in most programs; these features cut its work.

1. **Vertex array objects** (`glBindVertexArray` and friends).
2. **Instancing:** `glDrawArraysInstanced`, `glDrawElementsInstanced`,
   `glVertexAttribDivisor` (the control list's instanced draw packets).
3. **Uniform buffers:** `glBindBufferBase/Range`, `glUniformBlockBinding`,
   `glGetUniformBlockIndex`, block queries.
4. **Sampler objects** (`glGenSamplers`, ...): the sampler state record per
   unit from the sampler object when one is bound.
5. **Textures:** immutable storage (`glTexStorage2D/3D`), 3D and 2D array
   textures (`glTexImage3D`, `glTexSubImage3D`, `glCopyTexSubImage3D`), the
   sized internal formats of ES 3.0 (R, RG, integer, sRGB, 10:10:10:2,
   11:11:10, 9:9:9:5), texture swizzle, base and max level,
   `GL_TEXTURE_COMPARE_MODE` (shadow samplers).
6. **ETC2/EAC** (mandatory in ES 3.0): native in the TMU. ETC1, now decoded
   on the CPU, becomes native too (it is a subset of ETC2).
7. **Render targets beyond 8 bits:** 16- and 32-bit float, integer, sRGB
   (`job.c`'s tile load/store and clear per format; `EXT_color_buffer_float`
   in ES3 contexts; `glClearBuffer*`).
8. **Framebuffers:** `glBlitFramebuffer` (copy, scale, resolve: a render
   job or the TFU), `glInvalidateFramebuffer`, `glReadBuffer`,
   `glFramebufferTextureLayer`.
9. **Smaller ones:** sync objects (`glFenceSync`, `glClientWaitSync`,
   `glWaitSync`: the render-done counters), occlusion queries
   (`GL_ANY_SAMPLES_PASSED`: the V3D counters), primitive restart (fixed
   index), `glDrawRangeElements`, `glMapBufferRange`, `glCopyBufferSubData`,
   `glGetBufferParameteri64v`, `glGetInteger64v`, internal format queries.
10. **Tests:** `m8_es3` grows per feature; a new package `m8-1`; dEQP-GLES3
    groups as their features land (`functional.vertex_arrays`,
    `instanced`, `ubo`, `samplers`, `texture`, `fbo`, `sync`,
    `occlusion_query`, `primitive_restart`).

Done when: these dEQP-GLES3 groups pass on the A1200, GLES2 unchanged; **the
2.0 release** (§1).

## 6. Stage C: ES 3.0 complete (2-3 weeks)

1. **Transform feedback:** `glBeginTransformFeedback` and friends, the
   compiler's transform feedback outputs (a variant key), the V3D's
   transform feedback specs and buffers in the control list, pause and
   resume, the primitives-written query.
2. **Multisampled framebuffer objects:** `glRenderbufferStorageMultisample`
   (4x, the tile buffer as on the default framebuffer), resolve by
   `glBlitFramebuffer`.
3. **Pixel buffer objects:** `GL_PIXEL_PACK_BUFFER`, `GL_PIXEL_UNPACK_BUFFER`
   for `glReadPixels` and texture uploads.
4. **The rest:** `glGetInternalformativ`, program binary queries for ES3,
   `glProgramParameteri`, everything left of the 104.
5. **dEQP-GLES3's whole must-pass list** on the A1200; failures fixed or
   documented as deviations.

Done when: dEQP-GLES3 passes as well as dEQP-GLES2 does now.

## 7. Stage D: ES 3.1 (4-6 weeks)

1. **Compute shaders.** The largest single piece:
   - `v3dcore`: a CSD submission path beside the binning/render one, under
     the same GPU lock (`v3d_gpu_lock`), with its completion counter;
   - the compiler: compute stage variants (Mesa's compute key), `.v3dp` 1.1's
     compute stage;
   - the library: `glDispatchCompute`, `glDispatchComputeIndirect`, work
     group sizes and counts, shared memory, `glMemoryBarrier`.
2. **Shader storage buffers, image load/store, atomic counters:** the recipe
   types for their addresses and sizes, the GPU cache flushes between a
   writing job and a reading one, `glBindImageTexture`, the image formats.
3. **Indirect draws:** `glDrawArraysIndirect`, `glDrawElementsIndirect` (the
   control list's indirect packets).
4. **Textures:** multisample textures (`glTexStorage2DMultisample`,
   `texelFetch` per sample), `textureGather`, stencil texturing.
5. **Vertex attribute binding** (`glVertexAttribFormat`, `glBindVertexBuffer`,
   ...) and **separate shader objects** (program pipelines). Separate shader
   objects are the awkward one: a `.v3dp` is a linked vertex and fragment
   pair; a pipeline of separately compiled stages links them at draw time,
   so its variants are compiled for the pair when first drawn (stage A's
   path).
6. **Queries:** `glGetProgramInterfaceiv`, `glGetProgramResource*`,
   `glGetFramebufferParameteriv`, framebuffers without attachments.
7. **dEQP-GLES31** must-pass list on the A1200.

Done when: dEQP-GLES31 passes as well as GLES2 and GLES3 do; `GL_VERSION`
"OpenGL ES 3.1".

## 8. Risks

| Risk | Effect | Answer |
|---|---|---|
| Draw-time variants: a state change mid-game waits for the compiler | a stall of 0.1-0.5 s, once per new state | the cache keeps them; programs can ship their `.v3dp` with the variants they need, as now |
| More variant keys: more compiled variants per program | bigger cache files, longer first starts | variants only for states actually drawn (stage A's path), not precompiled for every state |
| GLSL ES 3.x compiles are slower on the 68k | longer first starts | the offline compiler, shipped binaries, the cache, as with GLES2 |
| Library size and memory | a bigger `ogles2.library` | measured per stage; ES3 code only runs in ES3 contexts |
| GLES2 regressions from shared code (job.c, draw.c, the reader) | breaks released programs | the GLES2 regression set at each stage's end; `main` stays releasable |
| Compute and storage writes: cache coherency | wrong results, rarely | Mesa's barriers and flushes followed exactly; dEQP-GLES31's memory groups |
| `main` and `gles3` drift apart | painful merges | merge `main` into `gles3` after each `main` release, and before each stage ends |

## 9. Order and checkpoints

| Stage | Contents | Estimate | Checkpoint |
|---|---|---|---|
| A | GLSL ES 3.00, ES3 contexts, draw-time variants | 2-3 weeks | `m8_es3`; dEQP-GLES2 unchanged |
| B | VAOs, instancing, UBOs, samplers, textures, render targets, ETC2, sync, queries, blit | 3-4 weeks | the dEQP-GLES3 groups; **release 2.0** |
| C | transform feedback, MSAA FBOs, PBOs, the rest | 2-3 weeks | dEQP-GLES3 complete |
| D | compute, SSBOs, images, atomics, indirect, gather, MS textures, SSO | 4-6 weeks | dEQP-GLES31 complete |

About 3-4 months in all. After stage B ogles2 covers most of what real ES3
programs use.

## 10. Open decisions

- The context tag's name and values (`OGLES2_CCT_PISTORM_ES_VERSION`, §3).
- Whether 2.0 waits for stage C (a complete ES 3.0) or goes out after B with
  the missing ES 3.0 parts listed as deviations (§1).
- Geometry shaders (`GL_EXT_geometry_shader`) after stage D, or not at all.
