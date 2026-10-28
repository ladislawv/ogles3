/*
 * Host check of ogles2/variant_key.c (docs/GLES3_PLAN.md stage A item 5):
 * the keys the library computes for a draw against the keys Mesa computed
 * for the same states, as v3dglslc stores them. For each program DIR/NAME
 * holds NAME.v3dp (the default key states, RGBA8 targets), NAME.v3dw (every
 * sampler returning 32 bits; programs with samplers) and NAME.v3da (alpha to
 * coverage, v3dglslc -a). Checked:
 *   - each fragment variant's key, computed from itself for its primitive,
 *     colour buffers, 32-bit samplers and alpha to coverage, is that key
 *     byte for byte (the fields the library sets are Mesa's in that state);
 *   - from the plain variant of the same primitive and colour buffers, the
 *     .v3da's and .v3dw's keys follow byte for byte from alpha to coverage
 *     and the 32-bit samplers alone;
 *   - a .v3dw variant serves a draw that wants fewer 32-bit samplers, a
 *     plain one none that wants more;
 *   - the vertex key computed from a fragment variant's inputs is that of
 *     the vertex variant drawn with it.
 *   usage: variant_key_test DIR NAME...    (`make variant-key-test`)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "variant_key.h"

#define FS_IN (4 + OGLES3_VK_FS_BYTES + 28 + 1) /* num_inputs, after the common prog data and uses_pid */
#define FS_SLOTS (4 + OGLES3_VK_FS_BYTES + 28 + 12)

struct variant {
    unsigned stage;
    const UBYTE *p; /* the section's payload: u32 stage, key, prog data, ... */
};

struct file {
    UBYTE *data;
    struct variant v[256];
    int n;
};

static int failures, checks;

static unsigned u32(const UBYTE *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static void check(int ok, const char *name, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s: %s\n", name, what);
    }
}

/* DIR/NAME.EXT's VARIANT sections; 0 if there is no such file. */
static int load(struct file *f, const char *dir, const char *name, const char *ext)
{
    char path[512];
    FILE *fp;
    long len, o;
    unsigned nsec, k;
    snprintf(path, sizeof(path), "%s/%s.%s", dir, name, ext);
    f->n = 0;
    f->data = 0;
    if (!(fp = fopen(path, "rb")))
        return 0;
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = malloc((size_t)len);
    if (!f->data || fread(f->data, 1, (size_t)len, fp) != (size_t)len || len < 32 || memcmp(f->data, "V3DP", 4)) {
        fclose(fp);
        printf("FAIL: %s: not a v3dp file\n", path);
        exit(1);
    }
    fclose(fp);
    nsec = u32(f->data + 24);
    for (k = 0, o = 32; k < nsec && o + 8 <= len; k++) {
        unsigned t = u32(f->data + o), n = u32(f->data + o + 4);
        if (t == 6 && f->n < 256) {
            f->v[f->n].stage = u32(f->data + o + 8);
            f->v[f->n].p = f->data + o + 8;
            f->n++;
        }
        o += 8 + n + (-n & 3);
    }
    return 1;
}

static const UBYTE *key(const struct variant *v)
{
    return v->p + 4;
}

/* A state stage A's draws give: no software blending, f32_color_rb, norm_16
 * or snorm, no target formats or blend state, the logic op COPY. Integer,
 * float and 16-bit normalised targets come with stage B (es3out's
 * precompiled variants are for an unsigned target). */
static int stage_a_state(const UBYTE *k)
{
    int i;
    if (k[20] || k[24] || k[25] || k[26] || u32(k + 288) != 12)
        return 0;
    for (i = 32; i < 288; i++)
        if (k[i])
            return 0;
    return 1;
}

static int prim_of(const UBYTE *k)
{
    return k[12] ? OGLES3_VK_POINTS : k[13] ? OGLES3_VK_LINES : OGLES3_VK_TRIANGLES;
}

/* The plain fragment variant of the same primitive and colour buffers. */
static const struct variant *plain_mate(const struct file *f, const UBYTE *k)
{
    int i;
    for (i = 0; i < f->n; i++)
        if (f->v[i].stage == 4 && key(&f->v[i])[12] == k[12] && key(&f->v[i])[13] == k[13] &&
            key(&f->v[i])[22] == k[22])
            return &f->v[i];
    return 0;
}

static void one(const char *dir, const char *name)
{
    struct file plain, wide, a2c;
    const struct file *files[3];
    UBYTE want[OGLES3_VK_FS_BYTES], vwant[OGLES3_VK_VS_BYTES], base[OGLES3_VK_VS_BYTES];
    int i, j, k, pairs = 0, skipped = 0, before = failures;
    if (!load(&plain, dir, name, "v3dp")) {
        printf("FAIL: %s: no %s/%s.v3dp\n", name, dir, name);
        failures++;
        return;
    }
    load(&wide, dir, name, "v3dw");
    load(&a2c, dir, name, "v3da");
    files[0] = &plain;
    files[1] = &wide;
    files[2] = &a2c;
    for (j = 0; j < 3; j++)
        for (i = 0; i < files[j]->n; i++) {
            const struct variant *v = &files[j]->v[i];
            const UBYTE *kk = key(v);
            const struct variant *m;
            if (v->stage != 4)
                continue;
            if (!stage_a_state(kk)) {
                skipped++;
                continue;
            }
            ogles2_vk_fs_want(want, kk, prim_of(kk), kk[22], ogles2_vk_bits(kk), j == 2);
            check(!memcmp(want, kk, OGLES3_VK_FS_BYTES), name, "a fragment key computed from itself is Mesa's");
            check(ogles2_vk_fs_fits(kk, want), name, "a fragment key fits itself");
            if (j == 0 || !(m = plain_mate(&plain, kk)))
                continue;
            ogles2_vk_fs_want(want, key(m), prim_of(kk), kk[22], ogles2_vk_bits(kk), j == 2);
            check(!memcmp(want, kk, OGLES3_VK_FS_BYTES), name,
                  j == 2 ? "the .v3da's key from the plain one and alpha to coverage"
                         : "the .v3dw's key from the plain one and its 32-bit samplers");
            if (j == 1) {
                ogles2_vk_fs_want(want, key(m), prim_of(kk), kk[22], ogles2_vk_bits(key(m)), 0);
                check(ogles2_vk_fs_fits(kk, want), name, "a .v3dw variant serves fewer 32-bit samplers");
                ogles2_vk_fs_want(want, key(m), prim_of(kk), kk[22], ogles2_vk_bits(kk), 0);
                check(ogles2_vk_bits(kk) == ogles2_vk_bits(key(m)) || !ogles2_vk_fs_fits(key(m), want), name,
                      "a plain variant does not serve more 32-bit samplers");
            }
            if (j == 2) {
                ogles2_vk_fs_want(want, key(m), prim_of(kk), kk[22], ogles2_vk_bits(kk), 0);
                check(!ogles2_vk_fs_fits(kk, want) && ogles2_vk_fs_fits(key(m), want), name,
                      "without alpha to coverage the plain variant serves, the .v3da's not");
            }
        }
    /* The vertex key from the fragment variant's inputs. */
    for (i = 0; i < plain.n; i++) {
        const UBYTE *f = plain.v[i].p;
        if (plain.v[i].stage != 4)
            continue;
        for (k = 0; k < plain.n; k++) {
            const UBYTE *vk = key(&plain.v[k]);
            if (plain.v[k].stage != 0 || vk[12] != f[FS_IN] || memcmp(vk + 16, f + FS_SLOTS, 64))
                continue;
            memcpy(base, vk, sizeof(base));
            base[12] = 0;
            memset(base + 16, 0, 64);
            ogles2_vk_vs_want(vwant, base, f[FS_IN], f + FS_SLOTS, ogles2_vk_bits(vk));
            check(!memcmp(vwant, vk, OGLES3_VK_VS_BYTES), name,
                  "the vertex key from the fragment variant's inputs is Mesa's");
            check(ogles2_vk_vs_fits(vk, vwant), name, "a vertex key fits itself");
            pairs++;
            break;
        }
    }
    check(pairs > 0, name, "a fragment variant with its vertex variant");
    printf("%s: %s, %d plain, %d 32-bit and %d alpha-to-coverage variants, %d vertex pairs%s\n",
           failures == before ? "ok" : "FAILED", name, plain.n, wide.n, a2c.n, pairs,
           skipped ? " (fragment keys of stage B's targets skipped)" : "");
    free(plain.data);
    free(wide.data);
    free(a2c.data);
}

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) {
        fprintf(stderr, "usage: variant_key_test DIR NAME...\n");
        return 2;
    }
    for (i = 2; i < argc; i++)
        one(argv[1], argv[i]);
    printf("%s: %d of %d checks\n", failures ? "FAILED" : "PASSED", checks - failures, checks);
    return failures ? 1 : 0;
}
