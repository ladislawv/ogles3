# `.v3dp` program binary container, version 1.0

**Status: frozen (2026-10-10).** Any change follows the versioning rules at the end. Verified:
the x86-64 and m68k writers produce identical bytes (`dev/spike-c/harness/compare_v3dp.sh`, five
test programs; the A1200 matched for the first four), and `v3dp_dump.py`, written from this
document, decodes every file.

A `.v3dp` file holds one linked GLES2 program compiled for V3D 4.2: every compiled shader
variant (QPU code, uniform recipe, the shader properties the draw path needs) and the program
tables the runtime needs to implement `glUniform*`, `glGetUniformLocation`, samplers and
attributes. It is written by the offline compiler (S1, `v3dglslc` on the PC) and by the
on-target compiler module (S2), and read by `ogles2.library` (plan sections 8.2 to 8.4).

**Rule zero: both compilers must produce the same bytes.** The writer serializes every field
explicitly; nothing is a memory image of a host struct (the hosts differ in byte order and in
struct alignment).

## Encoding

- All integers are **little-endian**. `u8`/`u16`/`u32`/`i16`/`i32`/`u64`.
- QPU instructions are `u64`, little-endian, exactly as the GPU reads them.
- Strings live in the string table (section `STRINGS`) and are referenced by `str`, a `u32`
  byte offset into it. Offset 0 is the empty string.
- Every section starts 4-byte aligned; padding bytes are 0.
- Floats are stored as their IEEE-754 bit patterns in `u32`.

## File layout

```
header
section 0
section 1
...
```

### Header (32 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | magic `"V3DP"` |
| 4 | `u16` | format major version (1) |
| 6 | `u16` | format minor version (0) |
| 8 | `u32` | V3D version × 10 + revision, as Mesa's `devinfo.ver` (42) |
| 12 | `char[12]` | Mesa commit, first 12 hex digits, NUL-padded |
| 24 | `u32` | number of sections |
| 28 | `u32` | CRC-32 (zlib's `crc32()`: reflected, polynomial 0xedb88320) of all bytes after the header |

A reader rejects a major version it does not know, a V3D version other than 42, and a Mesa
commit not on its list of validated commits (the runtime may then recompile with S2, or
report the miss for S1).

### Section

| Type | Field |
|---|---|
| `u32` | section type |
| `u32` | payload size in bytes (excluding padding) |
| bytes | payload, then zero padding to a multiple of 4 |

Unknown section types are skipped (forward compatibility within a major version).

| Type | Name | Count |
|---|---|---|
| 1 | `PROGRAM` | exactly 1, first |
| 2 | `STRINGS` | exactly 1, second |
| 3 | `STAGE_PARAMS` | one per linked stage (vertex, fragment) |
| 4 | `UNIFORMS` | exactly 1 |
| 5 | `ATTRIBUTES` | exactly 1 |
| 6 | `VARIANT` | one per compiled variant |

## `PROGRAM`

| Type | Field |
|---|---|
| `u8[32]` | BLAKE3 of the program's sources: for each shader in attach order, its stage (`u32`), its length in bytes (`u32`) and its text. A program linked with its attribute bindings (aliased attributes, `V3DGLSL_LINK_BOUND`) hashes them after the shaders the same way, as the stage `0x42494E44` ("BIND") with the text `<index> <name>
` per binding, sorted by name. The runtime's disk-cache file name is the hash's first 24 hex digits plus `.v3dp` (29 characters: FFS file names are at most 30); the runtime checks the full hash in the file |
| `u32` | stages present: bit 0 vertex, bit 4 fragment (Mesa's `mesa_shader_stage` bit numbers) |

## `STRINGS`

Concatenated NUL-terminated strings. The first byte is NUL (offset 0 = empty string).

## `STAGE_PARAMS`: one stage's constant buffer

The driver reads uniforms (`QUNIFORM_UNIFORM`) from a per-stage constant buffer of 32-bit
values: Mesa's `gl_program_parameter_list::ParameterValues`. The runtime keeps one per stage,
initialised from the template below, updates it from `glUniform*` (table `UNIFORMS`) and from
GL state before each draw (state entries below).

| Type | Field |
|---|---|
| `u32` | stage (0 vertex, 4 fragment) |
| `u32` | number of 32-bit values in the constant buffer (`NumParameterValues`) |
| `u32` | number of parameters (`NumParameters`) |
| per parameter: | |
| `u32` | kind: 0 uniform, 1 constant, 2 state variable (Mesa `PROGRAM_UNIFORM`, `PROGRAM_CONSTANT`, `PROGRAM_STATE_VAR`) |
| `str` | name |
| `u32` | value offset in the constant buffer, in 32-bit units (`ValueOffset`) |
| `u32` | size in components (`Size`) |
| `u32` | GL data type (`DataType`, e.g. `GL_FLOAT_VEC4`) |
| `i16[4]` | state indexes (`StateIndexes`, Mesa `gl_state_index` values); zeros unless kind 2 |
| `u32[n]` | template: the constant buffer's initial contents. Constants are filled in, uniforms hold the linker's initial values (0 in GLES2), state variables hold 0 |

The runtime must compute every state variable it is given before a draw, and must reject a
program containing a state index it does not implement (plan section 7.11), rather than leave
it at zero. State index numbers are Mesa's enum values at the validated commit, like the
recipe types below.

## `UNIFORMS`: the program's active uniforms

| Type | Field |
|---|---|
| `u32` | number of uniforms (Mesa's `UniformStorage` entries, in its order) |
| per uniform: | |
| `str` | name as Mesa stores it (an array of a basic type without `[0]`, struct members as `s.m`, `s[1].m`) |
| `u32` | GL type (`GL_FLOAT_MAT4`, `GL_SAMPLER_2D`, ...) of one element |
| `u32` | array elements (0 if not an array) |
| `u32` | flags: bit 0 sampler, bit 1 built-in (`gl_*`; a state variable, no storage places), bit 2 hidden (not visible to the application) |
| `u32` | number of storage places |
| per storage place: | where `glUniform*` writes the value; one per stage that uses the uniform |
| `u32` | stage |
| `u32` | offset in that stage's constant buffer, 32-bit units |
| `u8` | element stride in **bytes** (between array elements) |
| `u8` | vector stride in **bytes** (between matrix columns) |
| `u8` | format: 0 native, 1 integer stored as float (Mesa `gl_uniform_driver_format`) |
| `u8` | pad |
| `u32[2]` | sampler index for the vertex and the fragment stage (the texture index in that stage's `TMU_CONFIG` recipe data); 0xffffffff where not a sampler or unused there |

Samplers have no storage places: the texture unit a sampler reads (`glUniform1i`) is runtime
state, not part of a constant buffer. Built-in uniforms (`gl_DepthRange.near`, ...) have none
either; the shader reads them through state variables in `STAGE_PARAMS`.

Uniform locations are the runtime's own numbering; the file does not fix them. The strides
are the layout; with the V3D driver at the validated commit every vector, array element and
matrix column starts on 4 values (16 bytes), e.g. a `mat3` takes 12 values and a `vec3[4]` 16.

## `ATTRIBUTES`: vertex shader inputs

| Type | Field |
|---|---|
| `u32` | number of attributes |
| per attribute: | |
| `str` | name |
| `u32` | GL type |
| `u32` | GL location the linker assigned in bits 0-7 (informational). With bit 31 set, bits 8-11 say which of its columns the shader reads (bit k: column k; a vector or scalar: bit 0), each taking the next slot; a compiler older than that wrote the location alone, and then every column takes a slot |
| `u32` | input slot of its first column read: the number of vertex inputs the shader reads below it (`inputs_read` bit order, as `nir_recompute_io_bases` numbers them); 0xffffffff if the shader reads none of it |

The runtime maps an application's attribute arrays to input slots **by name**, so a binary
does not depend on `glBindAttribLocation`. A matrix attribute takes one slot per column.
The exception is aliasing (GLES 2.0 2.10.4: two names bound to one location). Mesa's linker
must know of it, or it gives every active attribute a location of its own and fails past
16. Such a program is linked with its bindings, and both names read the same slot.

## `VARIANT`: one compiled shader

| Type | Field |
|---|---|
| `u32` | stage: 0 vertex, 1 vertex binning (coordinate) shader, 4 fragment |
| key | the variant key, field by field (below) |
| prog data | the shader's properties, field by field (below) |
| `u32` | uniform recipe entries |
| `u32[n]`, `u32[n]` | recipe types (`enum quniform_contents` values), then their data words |
| `u32` | QPU code size in bytes |
| `u64[size/8]` | QPU instructions, little-endian |

Recipe type numbers are Mesa's enum values at the validated commit (0 `QUNIFORM_CONSTANT`
to 45 `QUNIFORM_BLEND_CONSTANT_A` at 74b69717c52b, and 46 `QUNIFORM_CLIP_Z_COEFF` from
`tools/v3dglslc/patches/0013`: the vertex position's z lerped towards w, -1 for
`EXT_clip_control`'s `ZERO_TO_ONE`, else 0; `v3dp_dump.py` lists them); the reader
checks the commit (header) and rejects types it does not implement (plan section 8.6). For
`QUNIFORM_UNIFORM` the data word is an index, in 32-bit units, into the stage's constant
buffer (`STAGE_PARAMS`); for `QUNIFORM_CONSTANT` it is the value itself.

Variants appear in the order the driver compiled them, without duplicates. The runtime uses
a variant only when its key equals the key the draw computes, field by field; anything else
is a miss (compile with S2, or report it).

What a file contains is the writer's choice, not part of the format. `v3dglslc` draws once
per default key state and stores what the driver compiles for those draws: triangles, lines
and points into an RGBA8 framebuffer object with a depth buffer, each sampler on its own unit
with a complete RGBA8 texture. That gives one vertex and one binning shader (two more if the
vertex shader writes `gl_PointSize`, for points) and a fragment shader per primitive class.
Variants the driver compiles at link time (`V3D_DEBUG=precompile`) are not stored: their keys
never match a draw (all eight colour buffers for `gl_FragColor`, every vertex output kept).

**`.v3dw`: the 32-bit-sampler variants** (2026-10-13; the format is unchanged). A program
with samplers has a second file of the same format, `<24 hex digits>.v3dw` beside its
`.v3dp`: the same draws with every sampler bound to an RGBA32F texture, so its fragment
variants have every sampler's bit set in `sampler_is_32b` (the TMU returns 32 bits a
channel, which float and depth textures need, and which reads 8-bit textures correctly too).
`ogles2.library` uses its fragment variants for draws that sample such a texture; its other
variants may be missing (`v3dglslc` captures what the driver compiles for those draws). The
offline compiler writes it next to the `.v3dp`; the on-target compiler writes it on request
(`V3DGLSL_LINK32`, `v3dp/v3dglsl_port.h`), when a draw first needs it.

**`.v3da`: the alpha-to-coverage variants** (2026-10-15; the format is unchanged). Every
program can have a third file of the same format, `<24 hex digits>.v3da`: the same draws with
the fragment key's `msaa` and `sample_alpha_to_coverage` set (`tools/v3dglslc/patches/0014`;
V3D has no fixed-function alpha to coverage, so the fragment shader turns alpha into the
sample mask). `ogles2.library` uses its fragment variants for draws into a 4x MSAA default
framebuffer with `GL_SAMPLE_ALPHA_TO_COVERAGE` enabled. Only the on-target compiler writes it,
on request (`V3DGLSL_LINK_A2C`), when a draw first needs it; it is looked for in `PROGDIR:`
and the cache first, as the `.v3dw` is.

### Common key and prog data (all stages)

Key (`struct v3d_key`): `u32 sampler_is_32b`, then `u8` each: `is_last_geometry_stage`,
`robust_uniform_access`, `robust_storage_access`, `robust_image_access`,
`robust_image_access_2`, `null_descriptor`, `u8 pad[2]`.

Prog data (`struct v3d_prog_data`): `u32` each: `spill_size`, `tmu_spills`, `tmu_fills`,
`tmu_count`, `qpu_read_stalls`; `u8` each: `compile_strategy_idx`, `threads`, `single_seg`,
`tmu_dirty_rcl`, `has_control_barrier`, `has_global_address`, `u8 pad[2]`.

### Vertex and binning shaders

Key (`struct v3d_vs_key`), after the common key: `u8 num_used_outputs`, `u8 pad[3]`,
`u8 used_outputs[64]` (`slot_and_component`), `u32 va_swap_rb_mask`, `u8` each: `is_coord`,
`per_vertex_point_size`, `clamp_color`, `u8 pad`.

Prog data (`struct v3d_vs_prog_data`), after the common part: `u8` each: `uses_iid`,
`uses_biid`, `uses_vid`, `separate_segments`, `vcm_cache_size`, `writes_psiz`, `u8 pad[2]`,
`u8 vattr_sizes[16]`, `u32 vpm_input_size`, `u32 vpm_output_size`,
`i32 driver_location_map[64]`.

### Fragment shader

Key (`struct v3d_fs_key`), after the common key: `u8` each: `is_points`, `is_lines`,
`line_smoothing`, `point_coord_upper_left`, `msaa`, `sample_alpha_to_coverage`,
`sample_alpha_to_one`, `can_earlyz_with_discard`, `software_blend`, `ignore_sample_mask`,
`cbufs`, `swap_color_rb`, `f32_color_rb`, `norm_16`, `snorm`, `ucp_enables`, `has_gs`,
`u8 pad[3]`; per draw buffer (8): `u32 format` (`enum pipe_format`), `u8 swizzle[4]`; per draw
buffer (8): `u32` each `rgb_func`, `rgb_src_factor`, `rgb_dst_factor`, `alpha_func`,
`alpha_src_factor`, `alpha_dst_factor`; `u32 logicop_func`, `u32 point_sprite_mask`.

Prog data (`struct v3d_fs_prog_data`), after the common part: `u8` each: `uses_pid`,
`num_inputs`, `writes_z`, `writes_z_from_fep`, `disable_ez`, `uses_center_w`,
`uses_implicit_point_line_varyings`, `lock_scoreboard_on_first_thrsw`,
`force_per_sample_msaa`, `u8 pad[3]`, `u8 input_slots[64]`, `u32 flat_shade_flags[3]`,
`u32 noperspective_flags[3]`, `u32 centroid_flags[3]`.

## Format 2: OpenGL ES 3.x programs

**Status: provisional (2026-10-22, docs/GLES3_PLAN.md §3).** Frozen with release 2.1, the
first library that writes it; until then the `gles3` builds write major version **0x8002**,
which every released reader refuses, and section numbers may still change.

A program is written in format 2 when one of its shaders is GLSL ES 3.00 or 3.10
(`#version 300 es`, `310 es`: Mesa's `GLSL_Version` 300 or more) or it was linked with ES3
link state (below). Every other program keeps format 1.0, byte for byte as above, so GLES2
programs' files stay readable by 1.x. A 1.x reader refuses format 2 at the header ("unknown
format major version"): an ES3 program's file read by it would otherwise draw wrongly (1.x
skips sections it does not know). A GLES2 context refuses a format 2 file from
`glProgramBinary`.

Everything not named here is as in 1.0: the encoding, the header (major 2, minor 0), the
section framing, and the sections `STRINGS`, `STAGE_PARAMS` and `VARIANT`. A reader takes up
to 256 sections (1.x: 64).

### Changed sections

**`PROGRAM`**, after the hash and the stages: `u32 features`, the must-understand feature
bits. A reader refuses a file with a bit it does not know, so a library never misreads a
program a newer one compiled (`glProgramBinary`, or a cache after a downgrade). Additions an
older reader may skip go into new sections and minor versions, as in 1.0. Bit 0 (0x1,
2026-10-27): transform feedback, the program has a `TRANSFORM_FEEDBACK` section (and its
binning shaders output the varyings fed back). Bit 1 (0x2, 2026-10-27, ES 3.1): shader
storage blocks, a `STORAGE_BLOCKS` section. Bit 2 (0x4, 2026-10-27, ES 3.1): a compute
program, its stages bit 5 alone (Mesa's `MESA_SHADER_COMPUTE`), its variants of stage 5. Bit 3
(0x8, 2026-10-27, ES 3.1): atomic counter buffers, an `ATOMIC_BUFFERS` section (with bit 1 and
its `STORAGE_BLOCKS` section: Mesa makes them storage buffers). The other ES 3.1 objects (stage
D: images, separable programs, explicit uniform locations) add theirs; a separable program may have one stage only (stage D).

**The hash** covers the program's link state after the shaders and the bindings, each as a
pseudo-stage written the way the bindings' `"BIND"` is (stage `u32`, length `u32`, text),
and only when the state is there:
- `0x58464256` (`"XFBV"`), transform feedback: the buffer mode as four lowercase hex digits
  (`8c8c` interleaved, `8c8d` separate) and `\n`, then each varying's name and `\n`, **in the
  order given** (it is the buffers' layout; `"BIND"` is sorted by name, this is not);
- `0x53455041` (`"SEPA"`), a separable program: the text `1`.

So two programs of the same sources and different link state never share a file.

**`UNIFORMS`**: a compute program's sampler words are its compute stage's index, then
0xffffffff. Each uniform after its two sampler words: `i32` block (its index in
`UNIFORM_BLOCKS`, -1 outside a block), `i32` offset in the block in bytes, `i32` array stride,
`i32` matrix stride (-1 each outside a block; Mesa's `gl_uniform_storage` fields), `u32` flags2:
bit 0 row-major; bit 1 (ES 3.1) a storage block's member, a buffer variable (its block an index
in `STORAGE_BLOCKS`; not a uniform: `GL_BUFFER_VARIABLE`'s); bit 2 (ES 3.1) an atomic counter
(its block an index in `ATOMIC_BUFFERS`, its offset and array stride in its buffer; a uniform
without a location). A block member has no storage
places.

**`ATTRIBUTES`**: the GL type may be an integer one (`GL_INT_VEC4`, `GL_UNSIGNED_INT`, ...)
or a non-square matrix (`GL_FLOAT_MAT2x3`, ...; its columns as for square ones). In the
location word, bit 30: the input has a location of its own (`layout(location)`, Mesa's
`explicit_location`); bits 0-7 are then that location, which the runtime keeps and
`glBindAttribLocation` cannot move. Without it the runtime numbers inputs as in 1.0.

### New sections

| Type | Name | Count |
|---|---|---|
| 7 | `UNIFORM_BLOCKS` | exactly 1 |
| 8 | `INTERFACE` | exactly 1 |
| 9 | `VARIANT_OF` | `.v3dx` files only, exactly 1, third |
| 10 | `TRANSFORM_FEEDBACK` | exactly 1 with feature bit 0, else none |
| 11 | `STORAGE_BLOCKS` | exactly 1 with feature bit 1, else none |
| 12 | `ATOMIC_BUFFERS` | exactly 1 with feature bit 3, else none |
| 13 | `UNIFORM_LOCATIONS` | at most 1 (format 2; written since 2026-10-27) |
| 14 | `OPAQUE_UNITS` | at most 1 (format 2; written since 2026-10-28) |

**`VARIANT`** of stage 5, a compute shader (feature bit 2): the common key (Mesa's
`v3d_update_compiled_cs` keys compute shaders by it alone), the common prog data, then
`u32 shared_size` (bytes of shared memory a work group uses), `u16 local_size[3]`,
`u8 can_use_supergroups`, `u8 pad`. `v3dglslc` compiles it with one dispatch of one work
group, its storage blocks bound to buffers.

**`STORAGE_BLOCKS`**: the program's shader storage blocks (Mesa's `ShaderStorageBlocks`),
then each stage's map to them, as `UNIFORM_BLOCKS` (the same fields per block, but each member
three `u32`s: its index in `UNIFORMS`, its top-level array's size and stride, Mesa's
`top_level_array_size` and `_stride`, for `GL_TOP_LEVEL_ARRAY_SIZE` and `_STRIDE`). A stage reads its storage buffer *i*
(`QUNIFORM_SSBO_OFFSET` and `QUNIFORM_GET_SSBO_SIZE` data *i*) as the program's block its
map's *i*-th entry names; the buffer is the one bound at that block's binding. An entry
`0x8000 | b` (past the stage's own blocks) is atomic counter buffer binding *b*: Mesa lowers
atomic counters to the storage slots after the stage's blocks (`nir_lower_atomics_to_ssbo`), the
buffer the range bound at `GL_ATOMIC_COUNTER_BUFFER` binding *b* (`st_bind_atomics`).

**`UNIFORM_LOCATIONS`**: the program's uniform locations as Mesa numbered them (its
`UniformRemapTable`: `layout(location)`'s, ES 3.1, among them; none for block members,
buffer variables, atomic counters and built-ins): `u32` count, then per location `u32` the
uniform's index in `UNIFORMS` (an array's elements on consecutive locations), `0xfffffffe` an
explicit location of a uniform that is not active (`glUniform*` there is ignored), `0xffffffff`
none (`GL_INVALID_OPERATION`). Without the section the runtime numbers the visible uniforms'
elements in `UNIFORMS` order, as format 1.0 does.

**`OPAQUE_UNITS`**: the units the program's samplers and images start with (`layout(binding)`,
ES 3.1; else 0): `u32` count, then per such uniform `u32` its index in `UNIFORMS`, `u32`
elements, `u32[n]` each one's unit (Mesa's storage). Without the section every one starts at 0.

**`ATOMIC_BUFFERS`**: `u32` count, then per buffer (Mesa's `AtomicBuffers`, in its order):
`u32` binding, `u32` minimum size in bytes, `u32` stages that reference it (bit per stage),
`u32` counters, then `u32[n]` their indices in `UNIFORMS`.

**`UNIFORM_BLOCKS`**: the program's uniform blocks, then each stage's map to them.

| Type | Field |
|---|---|
| `u32` | number of blocks (Mesa's `UniformBlocks`, in its order; an array of blocks is one block per element, `name[i]`) |
| per block: | |
| `str` | name |
| `u32` | initial binding (`Binding`: 0, or `layout(binding)`) |
| `u32` | data size in bytes (`UniformBufferSize`) |
| `u32` | stages that reference it (bit per stage, `stageref`) |
| `u32` | packing: 0 std140, 1 shared, 2 packed, 3 std430 (`_Packing`) |
| `u32` | active members, then `u32[n]` their indices in `UNIFORMS` |
| `u32` | number of stage maps, one per linked stage |
| per stage: | |
| `u32` | stage |
| `u32` | blocks the stage reads, then `u32[m]`: for its block *i* the program's block index |

A stage reads its block *i* as `QUNIFORM_UBO_ADDR` unit *i* + 1 (unit 0 is its own constant
buffer, as in 1.0) and `QUNIFORM_GET_UBO_SIZE` data *i*. A stage's order is not the
program's: the program's list appends each stage's blocks in stage order, so the map is
needed.

**`INTERFACE`**: the program's inputs and outputs, as Mesa's `ProgramResourceList` has them
(`GL_PROGRAM_INPUT` and `GL_PROGRAM_OUTPUT` entries, built-ins such as `gl_VertexID` and
`gl_FragDepth` included): `glGetFragDataLocation` now, and `glGetProgramResource*` and
program pipelines in stage D.

| Type | Field |
|---|---|
| `u32` | number of entries |
| per entry: | |
| `u32` | interface: `GL_PROGRAM_INPUT` (0x92E3) or `GL_PROGRAM_OUTPUT` (0x92E4) |
| `str` | name |
| `u32` | GL type of one element |
| `u32` | array elements (0 if not an array) |
| `i32` | location: an input's attribute location, an output's colour number (relative to `FRAG_RESULT_DATA0`); -1 for a built-in |
| `u8` | interpolation: 0 none, 1 smooth, 2 flat, 3 no perspective (`INTERP_MODE_*`) |
| `u8` | precision: 0 none, 1 high, 2 medium, 3 low (`GLSL_PRECISION_*`) |
| `u8` | flags: bit 0 explicit location |
| `u8` | index (an output's; 0 in ES) |
| `u32` | stages that reference it (bit per stage, `StageReferences`) |

**`TRANSFORM_FEEDBACK`**: a program linked with transform feedback varyings (its hash's
`"XFBV"`): the varyings as Mesa's linker laid them out (`LinkedTransformFeedback`, which
`glGetTransformFeedbackVarying` answers), then the V3D driver's layout of them for the binning
shader (`v3d_set_transform_feedback_outputs`, read from the driver's vertex shader state after
a draw).

| Type | Field |
|---|---|
| `u32` | buffer mode: `GL_INTERLEAVED_ATTRIBS` (0x8C8C) or `GL_SEPARATE_ATTRIBS` (0x8C8D) |
| `u32` | number of varyings, in the order given |
| per varying: | |
| `str` | name as given (`a[2]`: one element) |
| `u32` | GL type of one element |
| `u32` | elements (`Size`) |
| `u32` | buffer it is written to |
| `u32` | its offset in a vertex's record in that buffer, in bytes |
| `u32[4]` | each buffer's stride, in 32-bit words (0: unused) |
| `u32` | number of `TRANSFORM_FEEDBACK_OUTPUT_DATA_SPEC`s (at most 16) |
| `u16[n]` | the specs, packed as the `TRANSFORM_FEEDBACK_SPECS` packet takes them |
| `u16[n]` | the same for a draw of points whose vertex shader writes `gl_PointSize` (each first value one later) |
| pad | to 4 bytes |
| `u32` | number of the binning shader's outputs for them (at most 64) |
| `u8[n]` | each output's `slot_and_component` (slot × 4 + component), in VPM order after X, Y, Z, W, Xs, Ys (and the point size) |
| pad | to 4 bytes |

The binning shader variants of such a program are compiled with these outputs as their used
outputs (Mesa's `v3d_update_compiled_vs`), whether or not a draw feeds back.

### `.v3dx`: a program's draw-time variants

The variants compiled for draw states a program's own file lacks (docs/GLES3_PLAN.md stage A
item 5), in a file of their own: `<24 hex digits>.v3dx`, in the cache, or shipped beside the
program's file. Its format version is that of the program's file (1.0 for a GLES2 program).
Sections: `PROGRAM` (the program's), `STRINGS` (the empty string), `VARIANT_OF`, then
`VARIANT`s, at most 256 sections in all; a writer that would exceed that drops the oldest
variants.

**`VARIANT_OF`**: `u32` the CRC-32 (as the header's) of the program file's `STAGE_PARAMS`,
`UNIFORMS` and `ATTRIBUTES` sections, their headers and payloads in file order. A variant's
uniform recipe indexes the constant buffer as its program's link laid it out, so a runtime
takes variants only from a `.v3dx` whose CRC is that of the program file it loaded.

**Requests for variants** (`V3DGLSL_COMPILE_VARIANT`, `v3dp/v3dglsl_port.h`, and `v3dglslc -x`)
list keys as a `VARIANT` section begins: `u32` stage, then the key's bytes as above. A vertex
key's stage word may carry `V3DGLSL_KEY_OUTPUTS_FROM_FS` (0x100): its `num_used_outputs` and
`used_outputs` are then the inputs (`num_inputs`, `input_slots`) of the fragment variant
compiled just before it in the same list, as Mesa's `v3d_update_compiled_vs` takes them, so a
fragment variant and its vertex variant come in one request. The stored variant's key has the
plain stage value.

## Versioning

Adding a section type or appending fields at the end of a section is a minor version change.
Anything else is a major version change. The reader skips unknown sections and ignores
trailing bytes it does not know. A format 2 file may also carry feature bits (`PROGRAM`); a
reader refuses bits it does not know.
