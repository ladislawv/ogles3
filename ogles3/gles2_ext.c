/*
 * The GLES2 extensions of ES 3.0's features (docs/GLES3_PLAN.md §1,
 * ogles3.library 1.0): their entries, which call the ES 3.0 ones, those
 * accepting a GLES2 context while ctx->gles2_ext is set (ogles2_es3_entry).
 * The extensions' other parts (formats, targets, parameters, queries) are
 * open in GLES2 contexts where the ES 3.0 feature is (texture.c, fbo.c,
 * state.c, draw.c); context.c lists them.
 *   OES_texture_3D, EXT_texture_storage, EXT_map_buffer_range,
 *   EXT_occlusion_query_boolean, EXT_multi_draw_arrays
 */
#include "ogles3_internal.h"

#define AS_ES3(ctx, call)                  \
    do {                                   \
        if (!ogles2_context_valid(ctx))    \
            return;                        \
        ctx->gles2_ext = 1;                \
        call;                              \
        ctx->gles2_ext = 0;                \
    } while (0)

/* ---- OES_texture_3D (texture.c takes GL_TEXTURE_3D in GLES2 contexts) ---- */

void ogles2_glTexImage3DOES(struct OGLES3Context *ctx, GLenum target, GLint level, GLenum internalformat,
                            GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                            const void *pixels)
{
    ogles2_glTexImage3D(ctx, target, level, (GLint)internalformat, width, height, depth, border, format, type,
                        pixels);
}

void ogles2_glTexSubImage3DOES(struct OGLES3Context *ctx, GLenum target, GLint level, GLint xoffset,
                               GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                               GLenum format, GLenum type, const void *pixels)
{
    ogles2_glTexSubImage3D(ctx, target, level, xoffset, yoffset, zoffset, width, height, depth, format, type,
                           pixels);
}

void ogles2_glCopyTexSubImage3DOES(struct OGLES3Context *ctx, GLenum target, GLint level, GLint xoffset,
                                   GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    ogles2_glCopyTexSubImage3D(ctx, target, level, xoffset, yoffset, zoffset, x, y, width, height);
}

void ogles2_glCompressedTexImage3DOES(struct OGLES3Context *ctx, GLenum target, GLint level,
                                      GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth,
                                      GLint border, GLsizei imageSize, const void *data)
{
    ogles2_glCompressedTexImage3D(ctx, target, level, internalformat, width, height, depth, border, imageSize,
                                  data);
}

void ogles2_glCompressedTexSubImage3DOES(struct OGLES3Context *ctx, GLenum target, GLint level, GLint xoffset,
                                         GLint yoffset, GLint zoffset, GLsizei width, GLsizei height,
                                         GLsizei depth, GLenum format, GLsizei imageSize, const void *data)
{
    ogles2_glCompressedTexSubImage3D(ctx, target, level, xoffset, yoffset, zoffset, width, height, depth, format,
                                     imageSize, data);
}

/* A 3D texture's slice zoffset as an attachment (glFramebufferTextureLayer's). */
void ogles2_glFramebufferTexture3DOES(struct OGLES3Context *ctx, GLenum target, GLenum attachment,
                                      GLenum textarget, GLuint texture, GLint level, GLint zoffset)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (texture && textarget != GL_TEXTURE_3D) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    AS_ES3(ctx, ogles2_glFramebufferTextureLayer(ctx, target, attachment, texture, level, zoffset));
}

/* ---- EXT_texture_storage (texture.c's tex_storage takes GLES2 contexts) ---- */

void ogles2_glTexStorage2DEXT(struct OGLES3Context *ctx, GLenum target, GLsizei levels, GLenum internalformat,
                              GLsizei width, GLsizei height)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ogles2_glTexStorage2D(ctx, target, levels, internalformat, width, height);
}

void ogles2_glTexStorage3DEXT(struct OGLES3Context *ctx, GLenum target, GLsizei levels, GLenum internalformat,
                              GLsizei width, GLsizei height, GLsizei depth)
{
    if (!ogles2_context_valid(ctx))
        return;
    if (target != GL_TEXTURE_3D && !(ogles2_es3(ctx) && target == GL_TEXTURE_2D_ARRAY)) {
        ogles2_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ogles2_glTexStorage3D(ctx, target, levels, internalformat, width, height, depth);
}

/* ---- EXT_map_buffer_range ---- */

void *ogles2_glMapBufferRangeEXT(struct OGLES3Context *ctx, GLenum target, GLintptr offset, GLsizeiptr length,
                                 GLbitfield access)
{
    void *p;
    if (!ogles2_context_valid(ctx))
        return 0;
    ctx->gles2_ext = 1;
    p = ogles2_glMapBufferRange(ctx, target, offset, length, access);
    ctx->gles2_ext = 0;
    return p;
}

void ogles2_glFlushMappedBufferRangeEXT(struct OGLES3Context *ctx, GLenum target, GLintptr offset,
                                        GLsizeiptr length)
{
    AS_ES3(ctx, ogles2_glFlushMappedBufferRange(ctx, target, offset, length));
}

/* ---- EXT_occlusion_query_boolean: ES 3.0's queries, the two targets ---- */

static BOOL query_target(struct OGLES3Context *ctx, GLenum target)
{
    if (target == GL_ANY_SAMPLES_PASSED_EXT || target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE_EXT)
        return TRUE;
    ogles2_error(ctx, GL_INVALID_ENUM);
    return FALSE;
}

void ogles2_glGenQueriesEXT(struct OGLES3Context *ctx, GLsizei n, GLuint *ids)
{
    AS_ES3(ctx, ogles2_glGenQueries(ctx, n, ids));
}

void ogles2_glDeleteQueriesEXT(struct OGLES3Context *ctx, GLsizei n, const GLuint *ids)
{
    AS_ES3(ctx, ogles2_glDeleteQueries(ctx, n, ids));
}

GLboolean ogles2_glIsQueryEXT(struct OGLES3Context *ctx, GLuint id)
{
    GLboolean r;
    if (!ogles2_context_valid(ctx))
        return GL_FALSE;
    ctx->gles2_ext = 1;
    r = ogles2_glIsQuery(ctx, id);
    ctx->gles2_ext = 0;
    return r;
}

void ogles2_glBeginQueryEXT(struct OGLES3Context *ctx, GLenum target, GLuint id)
{
    if (ogles2_context_valid(ctx) && query_target(ctx, target))
        AS_ES3(ctx, ogles2_glBeginQuery(ctx, target, id));
}

void ogles2_glEndQueryEXT(struct OGLES3Context *ctx, GLenum target)
{
    if (ogles2_context_valid(ctx) && query_target(ctx, target))
        AS_ES3(ctx, ogles2_glEndQuery(ctx, target));
}

void ogles2_glGetQueryivEXT(struct OGLES3Context *ctx, GLenum target, GLenum pname, GLint *params)
{
    if (ogles2_context_valid(ctx) && query_target(ctx, target))
        AS_ES3(ctx, ogles2_glGetQueryiv(ctx, target, pname, params));
}

void ogles2_glGetQueryObjectuivEXT(struct OGLES3Context *ctx, GLuint id, GLenum pname, GLuint *params)
{
    AS_ES3(ctx, ogles2_glGetQueryObjectuiv(ctx, id, pname, params));
}

/* ---- EXT_multi_draw_arrays: one draw after another ---- */

void ogles2_glMultiDrawArraysEXT(struct OGLES3Context *ctx, GLenum mode, const GLint *first, const GLsizei *count,
                                 GLsizei primcount)
{
    GLsizei k;
    if (!ogles2_context_valid(ctx))
        return;
    if (primcount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; k < primcount; k++)
        ogles2_glDrawArrays(ctx, mode, first[k], count[k]);
}

void ogles2_glMultiDrawElementsEXT(struct OGLES3Context *ctx, GLenum mode, const GLsizei *count, GLenum type,
                                   const void *const *indices, GLsizei primcount)
{
    GLsizei k;
    if (!ogles2_context_valid(ctx))
        return;
    if (primcount < 0) {
        ogles2_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (k = 0; k < primcount; k++)
        ogles2_glDrawElements(ctx, mode, count[k], type, indices[k]);
}
