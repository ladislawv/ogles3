/*
 * The .v3dp reader (v3dp/FORMAT.md, version 1.0): a program binary from the
 * offline compiler (v3dglslc, S1) or the on-target one (S2) becomes a
 * struct OGLES2Image, with its code copied once into GPU memory.
 *
 * Every field is read explicitly, little-endian, with bounds checks; a file
 * the runtime cannot honour is refused with a reason rather than drawn
 * wrongly: an unknown major version, V3D version or Mesa commit, a bad CRC,
 * a uniform recipe type or state variable the runtime does not implement
 * (plan sections 8.3, 8.6, 7.11).
 */
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "ogles2_internal.h"
#include "v3d_device.h"
#include "v3d_context.h"
#include "v3d_frame.h"

/* Mesa commits whose recipe and state numbering the runtime implements. */
static const char *const validated_commits[] = {"74b69717c52b"};

/* ---- allocations: one list per image ---- */

static APTR alloc(struct OGLES2Image *im, ULONG size)
{
    APTR *b = AllocVec(size + 8, MEMF_ANY | MEMF_CLEAR);
    if (!b)
        return 0;
    b[0] = im->blocks; /* the list runs through the first word */
    im->blocks = b;
    return (UBYTE *)b + 8;
}

static void free_blocks(struct OGLES2Image *im)
{
    APTR *b = im->blocks, *next;
    while (b) {
        next = b[0];
        FreeVec(b);
        b = next;
    }
    im->blocks = 0;
}

/* ---- the reader ---- */

struct rd
{
    const UBYTE *p, *end;
    BOOL bad;
};

static ULONG need(struct rd *r, ULONG n)
{
    if (r->bad || (ULONG)(r->end - r->p) < n) {
        r->bad = TRUE;
        return 0;
    }
    return 1;
}

static UBYTE u8(struct rd *r)
{
    if (!need(r, 1))
        return 0;
    return *r->p++;
}

static UWORD u16(struct rd *r)
{
    UWORD v;
    if (!need(r, 2))
        return 0;
    v = (UWORD)(r->p[0] | (r->p[1] << 8));
    r->p += 2;
    return v;
}

static ULONG u32(struct rd *r)
{
    ULONG v;
    if (!need(r, 4))
        return 0;
    v = (ULONG)r->p[0] | ((ULONG)r->p[1] << 8) | ((ULONG)r->p[2] << 16) | ((ULONG)r->p[3] << 24);
    r->p += 4;
    return v;
}

static void skip(struct rd *r, ULONG n)
{
    if (need(r, n))
        r->p += n;
}

static ULONG crc32(const UBYTE *p, ULONG n)
{
    ULONG c = 0xffffffffUL;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320UL & (0UL - (c & 1)));
    }
    return ~c;
}

static void say(char *why, ULONG len, const char *a, const char *b, ULONG n)
{
    ULONG i = 0;
    const char *s;
    char num[12];
    int k = 0;
    for (s = a; *s && i + 1 < len; s++)
        why[i++] = *s;
    for (s = b; s && *s && i + 1 < len; s++)
        why[i++] = *s;
    if (n != ~0UL) {
        do
            num[k++] = (char)('0' + n % 10);
        while ((n /= 10) && k < 11);
        if (i + 1 < len)
            why[i++] = ' ';
        while (k && i + 1 < len)
            why[i++] = num[--k];
    }
    why[i] = 0;
}

/* A string table entry; "" for an offset outside the table. */
static const char *str_at(const char *strings, ULONG len, ULONG off)
{
    return off < len ? strings + off : "";
}

#define FAIL(a, b, n) do { say(why, whylen, (a), (b), (n)); goto fail; } while (0)

static BOOL recipe_ok(ULONG t)
{
    return t <= OGLES2_U_VIEWPORT_Z_SCALE || t == OGLES2_U_TMU_CONFIG_P0 || t == OGLES2_U_TMU_CONFIG_P1 ||
           (t >= OGLES2_U_TEXTURE_WIDTH && t <= OGLES2_U_TEXTURE_ARRAY_SIZE) ||
           t == OGLES2_U_UBO_ADDR || t == OGLES2_U_GET_UBO_SIZE ||
           t == OGLES2_U_LINE_WIDTH || t == OGLES2_U_AA_LINE_WIDTH ||
           t == OGLES2_U_SPILL_OFFSET || t == OGLES2_U_SPILL_SIZE_PER_THREAD ||
           (t >= OGLES2_U_BLEND_CONSTANT_R && t <= OGLES2_U_BLEND_CONSTANT_R + 3) || t == OGLES2_U_CLIP_Z_COEFF;
}

/* The columns of a matrix attribute (matCxR has C, each in a slot of its
 * own), 1 for the others. */
static UBYTE matrix_columns(GLenum t)
{
    switch (t) {
    case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: return 2;
    case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: return 3;
    case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: return 4;
    default: return 1;
    }
}

/* Format 2's major version, and the provisional one the gles3 builds write
 * until release 2.1 freezes it (FORMAT.md). */
#define FORMAT2      2
#define FORMAT2_PROV 0x8002
#define MAX_SECTIONS 256

/* The common prog data (FORMAT.md "Common key and prog data"), 28 bytes. */
static void read_common_prog_data(struct rd *r, struct OGLES2Variant *v)
{
    v->spill_size = u32(r);
    skip(r, 4 * 4);              /* tmu_spills, tmu_fills, tmu_count, qpu_read_stalls */
    skip(r, 1);                  /* compile_strategy_idx */
    v->threads = u8(r);
    v->single_seg = u8(r);
    skip(r, 3 + 2);              /* tmu_dirty_rcl, has_control_barrier, has_global_address, pad */
}

#define COMMON_KEY_BYTES (4 + 6 + 2) /* sampler_is_32b, six bools, pad */

struct OGLES2Image *ogles2_v3dp_load(const UBYTE *file, ULONG len, char *why, ULONG whylen)
{
    struct OGLES2Image *im = 0;
    struct rd r, s;
    ULONG nsections, i, k, stages = 0;
    const char *strings = 0;
    ULONG strings_len = 0;
    struct OGLES2Variant *vars = 0;
    ULONG nvars = 0, major;
    /* The section index: up to MAX_SECTIONS, allocated (a .v3dx may have
     * that many); freed at the end either way. */
    const UBYTE **sec_start = 0;
    ULONG *sec_type, *sec_len;
    BOOL commit_ok = FALSE, format2;
    BOOL have_blocks = FALSE, have_interface = FALSE;

    why[0] = 0;
    if (len < 32 || memcmp(file, "V3DP", 4))
        FAIL("not a .v3dp file", 0, ~0UL);
    r.p = file + 4;
    r.end = file + len;
    r.bad = FALSE;
    major = u16(&r);
    if (major != 1 && major != FORMAT2 && major != FORMAT2_PROV)
        FAIL("unknown format major version", 0, ~0UL);
    format2 = major != 1;
    (void)u16(&r);
    if (u32(&r) != 42)
        FAIL("not compiled for V3D 4.2", 0, ~0UL);
    for (i = 0; i < sizeof(validated_commits) / sizeof(validated_commits[0]); i++)
        if (!memcmp(r.p, validated_commits[i], 12))
            commit_ok = TRUE;
    if (!commit_ok)
        FAIL("compiled by a Mesa commit this library does not know", 0, ~0UL);
    r.p += 12;
    nsections = u32(&r);
    if (u32(&r) != crc32(file + 32, len - 32))
        FAIL("CRC mismatch (damaged file)", 0, ~0UL);
    if (nsections > MAX_SECTIONS)
        FAIL("too many sections:", 0, nsections);
    if (!(sec_start = AllocVec(nsections * 12 + 4, MEMF_ANY)))
        FAIL("out of memory", 0, ~0UL);
    sec_type = (ULONG *)(sec_start + nsections);
    sec_len = sec_type + nsections;

    /* Section index. */
    for (i = 0; i < nsections; i++) {
        sec_type[i] = u32(&r);
        sec_len[i] = u32(&r);
        sec_start[i] = r.p;
        skip(&r, (sec_len[i] + 3) & ~3UL);
        if (r.bad)
            FAIL("truncated section", 0, i);
        if (sec_type[i] == 6)
            nvars++;
    }
    if (nsections < 2 || sec_type[0] != 1 || sec_type[1] != 2)
        FAIL("PROGRAM and STRINGS must come first", 0, ~0UL);

    im = AllocVec(sizeof(*im), MEMF_ANY | MEMF_CLEAR);
    if (!im)
        FAIL("out of memory", 0, ~0UL);
    vars = alloc(im, (nvars ? nvars : 1) * sizeof(*vars));
    if (!vars)
        FAIL("out of memory", 0, ~0UL);
    strings = (const char *)sec_start[1];
    strings_len = sec_len[1];
    if (!strings_len || strings[strings_len - 1])
        FAIL("STRINGS not NUL-terminated", 0, ~0UL);

#define STR(off) str_at(strings, strings_len, (off))

    for (i = 0; i < nsections; i++) {
        s.p = sec_start[i];
        s.end = sec_start[i] + sec_len[i];
        s.bad = FALSE;
        switch (sec_type[i]) {
        case 1: /* PROGRAM */
            if (sec_len[i] < (format2 ? 40UL : 36UL))
                FAIL("short PROGRAM section", 0, ~0UL);
            memcpy(im->hash, s.p, 32);
            s.p += 32;
            stages = u32(&s);
            /* Format 2's must-understand features: none is known yet. */
            if (format2 && u32(&s))
                FAIL("the program uses features this library does not know", 0, ~0UL);
            break;
        case 3: { /* STAGE_PARAMS */
            ULONG stage = u32(&s), nvalues = u32(&s), nparams = u32(&s), si;
            ULONG *tmpl;
            struct OGLES2StateVar *sv;
            if (stage != 0 && stage != 4)
                FAIL("STAGE_PARAMS for an unknown stage", 0, stage);
            si = stage == 4;
            if (nvalues > 4096)
                FAIL("constant buffer too large:", 0, nvalues);
            /* Parameters: only state variables matter (the template holds the rest). */
            {
                const UBYTE *save = s.p;
                ULONG nstate = 0;
                for (k = 0; k < nparams; k++) {
                    if (u32(&s) == 2)
                        nstate++;
                    skip(&s, 4 * 4 + 8);
                }
                s.p = save;
                sv = alloc(im, (im->nstatevars + nstate + 1) * sizeof(*sv));
                if (!sv)
                    FAIL("out of memory", 0, ~0UL);
                if (im->nstatevars)
                    memcpy(sv, im->statevars, im->nstatevars * sizeof(*sv));
            }
            for (k = 0; k < nparams; k++) {
                ULONG kind = u32(&s), name = u32(&s), off = u32(&s), size = u32(&s);
                UWORD st;
                (void)u32(&s); /* GL type */
                st = u16(&s);
                skip(&s, 6);
                (void)name;
                (void)size;
                if (kind == 2) {
                    if (st != OGLES2_STATE_DEPTH_RANGE && st != OGLES2_STATE_FB_WPOS_Y_TRANSFORM &&
                        st != OGLES2_STATE_FB_PNTC_Y_TRANSFORM)
                        FAIL("state variable not implemented: ", STR(name), st);
                    if (off + 4 > nvalues)
                        FAIL("state variable outside the constant buffer", 0, ~0UL);
                    sv[im->nstatevars].stage = (UBYTE)si;
                    sv[im->nstatevars].offset = (UWORD)off;
                    sv[im->nstatevars].state = st;
                    im->nstatevars++;
                }
            }
            im->statevars = sv;
            tmpl = alloc(im, nvalues * 4 + 4);
            if (!tmpl)
                FAIL("out of memory", 0, ~0UL);
            for (k = 0; k < nvalues; k++)
                tmpl[k] = u32(&s);
            im->cbuf_words[si] = (UWORD)nvalues;
            im->cbuf_template[si] = tmpl;
            break;
        }
        case 4: { /* UNIFORMS */
            ULONG n = u32(&s);
            struct OGLES2ImageUniform *u;
            if (n > 1024)
                FAIL("too many uniforms:", 0, n);
            u = alloc(im, (n + 1) * sizeof(*u));
            if (!u)
                FAIL("out of memory", 0, ~0UL);
            for (k = 0; k < n; k++) {
                ULONG np, j;
                struct OGLES2Place *pl;
                u[k].name = STR(u32(&s));
                u[k].type = u32(&s);
                u[k].array = (UWORD)u32(&s);
                u[k].flags = (UBYTE)u32(&s);
                np = u32(&s);
                if (np > 2)
                    FAIL("uniform with more than two storage places: ", u[k].name, np);
                pl = alloc(im, (np + 1) * sizeof(*pl));
                if (!pl)
                    FAIL("out of memory", 0, ~0UL);
                for (j = 0; j < np; j++) {
                    ULONG stage = u32(&s);
                    pl[j].stage = stage == 4;
                    pl[j].offset = (UWORD)u32(&s);
                    pl[j].elem_stride = (UBYTE)(u8(&s) / 4);
                    pl[j].vec_stride = (UBYTE)(u8(&s) / 4);
                    pl[j].int_as_float = u8(&s);
                    (void)u8(&s);
                }
                u[k].nplaces = (UBYTE)np;
                u[k].places = pl;
                u[k].sampler[0] = u32(&s);
                u[k].sampler[1] = u32(&s);
                u[k].block = -1;
                if (format2) {
                    ULONG b = u32(&s);
                    u[k].offset = (LONG)u32(&s);
                    u[k].array_stride = (LONG)u32(&s);
                    u[k].matrix_stride = (LONG)u32(&s);
                    u[k].row_major = (UBYTE)(u32(&s) & 1);
                    if (b != 0xffffffffUL) {
                        if (b > 0x7fff || np)
                            FAIL("a uniform block member that is not one: ", u[k].name, b);
                        u[k].block = (WORD)b;
                    }
                }
            }
            im->nuniforms = (UWORD)n;
            im->uniforms = u;
            break;
        }
        case 5: { /* ATTRIBUTES */
            ULONG n = u32(&s);
            struct OGLES2ImageAttrib *a;
            if (n > OGLES2_MAX_IMAGE_ATTRIBS)
                FAIL("too many attributes:", 0, n);
            a = alloc(im, (n + 1) * sizeof(*a));
            if (!a)
                FAIL("out of memory", 0, ~0UL);
            for (k = 0; k < n; k++) {
                ULONG slot, loc;
                a[k].name = STR(u32(&s));
                a[k].type = u32(&s);
                loc = u32(&s); /* the linker's location (informational); its columns read */
                slot = u32(&s);
                a[k].slot = slot > 15 ? 0xff : (UBYTE)slot;
                a[k].columns = matrix_columns(a[k].type);
                a[k].colmask = (loc & 0x80000000UL) ? (UBYTE)((loc >> 8) & 15) : 0;
                /* Format 2, bit 30: a location of its own (layout(location)). */
                a[k].location = 0xff;
                if (format2 && (loc & 0x40000000UL)) {
                    if ((loc & 0xff) + a[k].columns > OGLES2_MAX_VERTEX_ATTRIBS)
                        FAIL("an attribute location past the last: ", a[k].name, loc & 0xff);
                    a[k].location = (UBYTE)(loc & 0xff);
                }
            }
            im->nattribs = (UBYTE)n;
            im->attribs = a;
            break;
        }
        case 6: { /* VARIANT */
            struct OGLES2Variant *v = &vars[im->nvariants];
            ULONG n, j, codelen;
            ULONG *rt, *rd2;
            v->stage = (UBYTE)u32(&s);
            if (v->stage == OGLES2_VARIANT_VS || v->stage == OGLES2_VARIANT_CS) {
                skip(&s, COMMON_KEY_BYTES);
                skip(&s, 4 + 64 + 4);             /* num_used_outputs+pad, used_outputs, va_swap_rb_mask */
                (void)u8(&s);                     /* is_coord (the stage says it) */
                v->per_vertex_point_size = u8(&s);
                skip(&s, 2);                      /* clamp_color, pad */
                read_common_prog_data(&s, v);
                v->uses_iid = u8(&s);
                (void)u8(&s);                     /* uses_biid */
                v->uses_vid = u8(&s);
                v->separate_segments = u8(&s);
                v->vcm_cache_size = u8(&s);
                v->writes_psiz = u8(&s);
                skip(&s, 2);
                for (j = 0; j < 16; j++)
                    v->vattr_sizes[j] = u8(&s);
                v->vpm_input_size = u32(&s);
                v->vpm_output_size = u32(&s);
                skip(&s, 64 * 4);                 /* driver_location_map */
            } else if (v->stage == OGLES2_VARIANT_FS) {
                skip(&s, COMMON_KEY_BYTES);
                v->is_points = u8(&s);
                v->is_lines = u8(&s);
                skip(&s, 8);                      /* line_smoothing .. ignore_sample_mask */
                v->cbufs = u8(&s);
                skip(&s, 6 + 3);                  /* the other key bytes, pad */
                skip(&s, 8 * 8 + 8 * 24 + 4 + 4); /* colour formats, blend, logicop, sprite mask */
                read_common_prog_data(&s, v);
                v->uses_pid = u8(&s);
                v->num_inputs = u8(&s);
                v->writes_z = u8(&s);
                v->writes_z_from_fep = u8(&s);
                v->disable_ez = u8(&s);
                v->uses_center_w = u8(&s);
                v->uses_implicit_point_line_varyings = u8(&s);
                v->lock_scoreboard_on_first_thrsw = u8(&s);
                skip(&s, 1 + 3 + 64);             /* force_per_sample_msaa, pad, input_slots */
                for (j = 0; j < 3; j++)
                    v->flat_shade_flags[j] = u32(&s);
                for (j = 0; j < 3; j++)
                    v->noperspective_flags[j] = u32(&s);
                for (j = 0; j < 3; j++)
                    v->centroid_flags[j] = u32(&s);
            } else {
                FAIL("variant of an unknown stage", 0, v->stage);
            }
            n = u32(&s);
            if (n > 4096)
                FAIL("uniform recipe too long:", 0, n);
            rt = alloc(im, n * 8 + 8);
            if (!rt)
                FAIL("out of memory", 0, ~0UL);
            rd2 = rt + n;
            for (j = 0; j < n; j++) {
                rt[j] = u32(&s);
                if (!recipe_ok(rt[j]))
                    FAIL("uniform recipe type not implemented yet:", 0, rt[j]);
            }
            for (j = 0; j < n; j++) {
                rd2[j] = u32(&s);
                /* UBO 0 is the stage's own constant buffer (indirectly
                 * indexed uniform arrays); others are uniform blocks of
                 * format 2's programs (checked against the stage's map
                 * below). */
                if (rt[j] == OGLES2_U_UBO_ADDR && (rd2[j] >> 24) != 0 && !format2)
                    FAIL("a uniform block other than the default one", 0, rd2[j] >> 24);
                if (rt[j] == OGLES2_U_UBO_ADDR)
                    v->uses_ubo = 1;
                if (rt[j] == OGLES2_U_UBO_ADDR && (rd2[j] >> 24) != 0)
                    v->uses_blocks = 1;
                if (rt[j] == OGLES2_U_CLIP_Z_COEFF)
                    im->clip_z_coeff = TRUE;
            }
            v->nrecipe = (UWORD)n;
            v->rtype = rt;
            v->rdata = rd2;
            {
                UWORD *ui = alloc(im, n * 4 + 4), *sp = ui ? ui + n : 0, ns = 0;
                if (!ui)
                    FAIL("out of memory", 0, ~0UL);
                for (j = 0; j < n; j++) {
                    if (rt[j] == OGLES2_U_UNIFORM && rd2[j] < 65536) {
                        ui[j] = (UWORD)rd2[j];
                    } else {
                        ui[j] = 0;
                        sp[ns++] = (UWORD)j;
                    }
                }
                v->uidx = ui;
                v->special = sp;
                v->nspecial = ns;
                v->no_textures = 1;
                for (j = 0; j < n; j++)
                    if (rt[j] == OGLES2_U_TMU_CONFIG_P0 || rt[j] == OGLES2_U_TMU_CONFIG_P1 ||
                        (rt[j] >= OGLES2_U_TEXTURE_WIDTH && rt[j] <= OGLES2_U_TEXTURE_ARRAY_SIZE) ||
                        rt[j] == OGLES2_U_UBO_ADDR || rt[j] == OGLES2_U_GET_UBO_SIZE || rt[j] == OGLES2_U_SPILL_OFFSET ||
                        (rt[j] >= OGLES2_U_BLEND_CONSTANT_R && rt[j] <= OGLES2_U_BLEND_CONSTANT_R + 3))
                        v->no_textures = 0; /* nor the blend colour or the spill area: not in draw.c's key */
            }
            codelen = u32(&s);
            if ((codelen & 7) || !need(&s, codelen))
                FAIL("bad code size", 0, codelen);
            v->code = s.p;
            v->code_bytes = codelen;
            s.p += codelen;
            if (s.bad)
                FAIL("truncated VARIANT section", 0, ~0UL);
            im->nvariants++;
            break;
        }
        case 7: { /* UNIFORM_BLOCKS (format 2) */
            ULONG n, nmaps, j;
            struct OGLES2ImageBlock *b;
            if (!format2)
                break;
            n = u32(&s);
            if (n > 1024)
                FAIL("too many uniform blocks:", 0, n);
            if (!(b = alloc(im, (n + 1) * sizeof(*b))))
                FAIL("out of memory", 0, ~0UL);
            for (k = 0; k < n; k++) {
                UWORD *m;
                ULONG nm;
                b[k].name = STR(u32(&s));
                b[k].binding = u32(&s);
                b[k].size = u32(&s);
                b[k].stages = (UBYTE)u32(&s);
                b[k].packing = (UBYTE)u32(&s);
                nm = u32(&s);
                if (nm > 4096 || !(m = alloc(im, nm * 2 + 2)))
                    FAIL("a uniform block's members: ", b[k].name, nm);
                for (j = 0; j < nm; j++)
                    m[j] = (UWORD)u32(&s);
                b[k].nmembers = (UWORD)nm;
                b[k].members = m;
            }
            im->nublocks = (UWORD)n;
            im->ublocks = b;
            nmaps = u32(&s);
            for (k = 0; k < nmaps && !s.bad; k++) {
                ULONG stage = u32(&s), nb = u32(&s), si;
                UWORD *map;
                if (stage != 0 && stage != 4)
                    FAIL("a uniform block map for an unknown stage", 0, stage);
                si = stage == 4;
                if (nb > 255 || !(map = alloc(im, nb * 2 + 2)))
                    FAIL("a stage's uniform blocks:", 0, nb);
                for (j = 0; j < nb; j++) {
                    map[j] = (UWORD)u32(&s);
                    if (map[j] >= n)
                        FAIL("a stage's uniform block past the program's last", 0, map[j]);
                }
                im->stage_nblocks[si] = (UBYTE)nb;
                im->stage_block[si] = map;
            }
            have_blocks = TRUE;
            break;
        }
        case 8: { /* INTERFACE (format 2) */
            ULONG n;
            struct OGLES2ImageVar *v;
            if (!format2)
                break;
            n = u32(&s);
            if (n > 1024)
                FAIL("too many inputs and outputs:", 0, n);
            if (!(v = alloc(im, (n + 1) * sizeof(*v))))
                FAIL("out of memory", 0, ~0UL);
            for (k = 0; k < n; k++) {
                v[k].iface = u32(&s);
                v[k].name = STR(u32(&s));
                v[k].type = u32(&s);
                v[k].array = (UWORD)u32(&s);
                v[k].location = (WORD)(LONG)u32(&s);
                v[k].interpolation = u8(&s);
                v[k].precision = u8(&s);
                v[k].flags = u8(&s);
                v[k].index = u8(&s);
                v[k].stages = (UBYTE)u32(&s);
                if (v[k].iface != GL_PROGRAM_INPUT && v[k].iface != GL_PROGRAM_OUTPUT)
                    FAIL("an interface entry of no known interface: ", v[k].name, v[k].iface);
            }
            im->ninterface = (UWORD)n;
            im->interface = v;
            have_interface = TRUE;
            break;
        }
        default:
            break; /* STRINGS, and sections from newer minor versions */
        }
        if (s.bad)
            FAIL("truncated section", 0, i);
    }
    if (stages != 0x11)
        FAIL("not a vertex and fragment program", 0, ~0UL);
    if (format2 && (!have_blocks || !have_interface))
        FAIL("a format 2 file without UNIFORM_BLOCKS or INTERFACE", 0, ~0UL);
    im->format2 = format2;
    /* Uniform blocks the variants read must be the stage's (its map). */
    for (k = 0; k < im->nvariants; k++) {
        const struct OGLES2Variant *v = &vars[k];
        ULONG j, si = v->stage == OGLES2_VARIANT_FS;
        for (j = 0; j < v->nrecipe; j++)
            if ((v->rtype[j] == OGLES2_U_UBO_ADDR && (v->rdata[j] >> 24) > im->stage_nblocks[si]) ||
                (v->rtype[j] == OGLES2_U_GET_UBO_SIZE && v->rdata[j] >= im->stage_nblocks[si]))
                FAIL("a variant reads a uniform block its stage does not have", 0, v->rdata[j]);
    }
    im->variants = vars;

    /* The code: every variant, 8-byte aligned, in one block of GPU memory;
     * without a GPU (a state-only context: tests) in ordinary memory, never
     * drawn with. */
    if (!ogles2_device_ready()) {
        ULONG off = 0;
        UBYTE *c;
        for (k = 0; k < im->nvariants; k++) {
            vars[k].code_offset = off;
            off += vars[k].code_bytes;
        }
        if (!(c = alloc(im, off + 8)))
            FAIL("out of memory", 0, ~0UL);
        for (k = 0; k < im->nvariants; k++) {
            memcpy(c + vars[k].code_offset, vars[k].code, vars[k].code_bytes);
            vars[k].code = c + vars[k].code_offset;
        }
        im->code_total = off;
    } else {
        V3DDevice *dev = ogles2_device();
        struct v3d_mem *m = alloc(im, sizeof(*m));
        ULONG off = 0, clen;
        if (!m)
            FAIL("out of memory", 0, ~0UL);
        for (k = 0; k < im->nvariants; k++) {
            vars[k].code_offset = off;
            off += vars[k].code_bytes;
        }
        if (!off || v3d_mem_alloc(dev, m, off) < 0)
            FAIL("out of GPU memory for the code", 0, ~0UL);
        im->code_mem = m;
        im->code_total = off;
        for (k = 0; k < im->nvariants; k++) {
            memcpy((UBYTE *)m->hostptr + vars[k].code_offset, vars[k].code, vars[k].code_bytes);
            vars[k].code = (const UBYTE *)m->hostptr + vars[k].code_offset;
        }
        clen = m->size;
        CachePreDMA(m->hostptr, &clen, 0);
    }
    /* Names: STRINGS points into `file`, which goes; keep a copy. */
    {
        char *copy = alloc(im, strings_len);
        ULONG d = (ULONG)copy - (ULONG)strings;
        if (!copy)
            FAIL("out of memory", 0, ~0UL);
        memcpy(copy, strings, strings_len);
        for (k = 0; k < im->nattribs; k++)
            ((struct OGLES2ImageAttrib *)im->attribs)[k].name += d;
        for (k = 0; k < im->nuniforms; k++)
            ((struct OGLES2ImageUniform *)im->uniforms)[k].name += d;
        for (k = 0; k < im->nublocks; k++)
            ((struct OGLES2ImageBlock *)im->ublocks)[k].name += d;
        for (k = 0; k < im->ninterface; k++)
            ((struct OGLES2ImageVar *)im->interface)[k].name += d;
    }
    im->name = "program binary";
    FreeVec(sec_start);
    return im;

fail:
    if (sec_start)
        FreeVec(sec_start);
    if (im) {
        if (im->code_mem && im->code_mem->hostptr)
            v3d_mem_free(ogles2_device(), im->code_mem);
        free_blocks(im);
        FreeVec(im);
    }
    return 0;
}

void ogles2_image_free(struct OGLES2Image *im, struct OGLES2Context *ctx)
{
    if (!im)
        return;
    if (im->code_mem && im->code_mem->hostptr) {
        /* The GPU may still run this code in the open job: free it after
         * that job (v3d_frame_defer_free), or now. */
        if (ctx && ctx->v3d && ctx->v3d->frame_active)
            v3d_frame_defer_free(&ctx->v3d->frame, im->code_mem);
        else
            v3d_mem_free(ogles2_device(), im->code_mem);
    }
    if (im->binary)
        FreeVec(im->binary);
    ogles2_image_free(im->wide, ctx); /* its code: deferred the same way */
    ogles2_image_free(im->a2c, ctx);
    if (im->vs_src)
        FreeVec(im->vs_src);
    if (im->fs_src)
        FreeVec(im->fs_src);
    free_blocks(im);
    FreeVec(im);
}

/* The variant for a draw: the stage's variant whose primitive key matches
 * (fragment: points, lines or triangles, and the render targets it writes,
 * `cbufs`; vertex and binning: a point size written per vertex, for points,
 * when the program has such a variant). A fragment shader with no variant
 * for those targets: the one for target 0 (EXT_draw_buffers: v3dglslc
 * makes the others only for programs that write gl_FragData[1] or up). */
const struct OGLES2Variant *ogles2_image_variant(const struct OGLES2Image *im, UBYTE stage, UBYTE cbufs,
                                                 GLenum mode)
{
    const struct OGLES2Variant *v, *any = 0, *rt0 = 0;
    BOOL points = mode == GL_POINTS;
    BOOL lines = mode == GL_LINES || mode == GL_LINE_LOOP || mode == GL_LINE_STRIP;
    UWORD i;
    for (i = 0; i < im->nvariants; i++) {
        v = &im->variants[i];
        if (v->stage != stage)
            continue;
        if (stage == OGLES2_VARIANT_FS) {
            if (v->is_points == points && v->is_lines == lines) {
                UBYTE vc = v->cbufs ? v->cbufs : 1;
                if (vc == cbufs)
                    return v;
                if (vc == 1 && !rt0)
                    rt0 = v;
            }
        } else {
            if (v->per_vertex_point_size == (points ? 1 : 0))
                return v;
        }
        if (!any)
            any = v;
    }
    return rt0 ? rt0 : any; /* a miss: the closest there is (plan 7.8); draw.c logs it */
}
