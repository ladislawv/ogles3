/*
 * dEQP-GLES2 and dEQP-GLES3 on AmigaOS 3 (ports/deqp): the platform. A render
 * context is an ogles2 context, OpenGL ES 2.0 or 3.0 as the test package asks
 * (3.0 from an ES3 build of the library: docs/GLES3_PLAN.md stage A item 6),
 * with its window on the Workbench screen (whatever the surface type asked
 * for: ogles2 has no pbuffers), RGBA8 with depth 24 and stencil 8, 4x
 * multisampled when the configuration asks for samples
 * (--deqp-gl-config-name=rgba8888d24s8ms4), no vsync. The GL functions,
 * core and extensions, are aglGetProcAddress's thunks, which call the
 * current context through the library's dispatch table. Ctrl-C stops the
 * run between tests.
 */
#include "tcuPlatform.hpp"
#include "tcuDefs.hpp"
#include "tcuRenderTarget.hpp"
#include "tcuCommandLine.hpp"
#include "gluPlatform.hpp"
#include "gluContextFactory.hpp"
#include "gluRenderContext.hpp"
#include "gluRenderConfig.hpp"
#include "glwFunctions.hpp"
#include "glwFunctionLoader.hpp"
#include "glwInitFunctions.hpp"
#include "glwEnums.hpp"
#include "deStringUtil.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <libraries/ogles3.h>

/* libogles3.a (built by the library's own toolchain; plain C calls). */
extern "C" {
BOOL OGLES3Open(void);
void OGLES3Close(void);
void *aglCreateContextTags2(ULONG *errcode, ...);
void aglSetParamsTags2(ULONG tag1, ...);
void aglDestroyContext(void *context);
void aglMakeCurrent(void *context);
void aglSwapBuffers(void);
void *aglGetProcAddress(const char *name);
}

namespace tcu
{
namespace agl
{

/* The functions an ES 3.0 context's table names that the library does not
 * have yet (docs/GLES3_PLAN.md stages B and C): a null pointer there is a
 * jump to address 0 at the first call, and glu::resetState calls many of
 * them after every case. Each gets a stub of its own instead, which does
 * nothing, returns 0 and says once which function it stands for. A throw
 * would end the run: an exception in a case's deinit terminates dEQP. The
 * stub takes no arguments: the 68k caller pops its own, so any signature
 * returning an integer or a pointer (in d0) is safe. */
enum
{
    MAX_MISSING = 256
};
static const char *s_missing[MAX_MISSING];
static bool s_missingSaid[MAX_MISSING];
static int s_numMissing;

template <int N>
static long missingFunction(void)
{
    if (!s_missingSaid[N])
    {
        s_missingSaid[N] = true;
        printf("ogles3.library: %s is not implemented yet (called as a no-op)\n", s_missing[N]);
        fflush(stdout);
    }
    return 0;
}

template <int... N>
static glw::GenericFuncType missingStub(int i, std::integer_sequence<int, N...>)
{
    static const glw::GenericFuncType stubs[] = {(glw::GenericFuncType)&missingFunction<N>...};
    return stubs[i];
}

/* stubs: an ES 3.0 table's (above); an ES 2.0 one keeps null pointers, as
 * dEQP-GLES2 has passed with. The names are the CTS's string literals. */
class FunctionLoader : public glw::FunctionLoader
{
public:
    FunctionLoader(bool stubs) : m_stubs(stubs)
    {
    }
    glw::GenericFuncType get(const char *name) const override
    {
        glw::GenericFuncType fn = (glw::GenericFuncType)aglGetProcAddress(name);
        if (fn || !m_stubs || s_numMissing == MAX_MISSING)
            return fn;
        for (int i = 0; i < s_numMissing; i++)
            if (!strcmp(s_missing[i], name))
                return missingStub(i, std::make_integer_sequence<int, MAX_MISSING>());
        s_missing[s_numMissing] = name;
        return missingStub(s_numMissing++, std::make_integer_sequence<int, MAX_MISSING>());
    }

private:
    bool m_stubs;
};

class RenderContext : public glu::RenderContext
{
public:
    RenderContext(const glu::RenderConfig &config);
    ~RenderContext(void) override;

    glu::ContextType getType(void) const override
    {
        return m_type;
    }
    const glw::Functions &getFunctions(void) const override
    {
        return m_gl;
    }
    const tcu::RenderTarget &getRenderTarget(void) const override
    {
        return m_target;
    }
    void postIterate(void) override
    {
        aglSwapBuffers();
    }
    glw::GenericFuncType getProcAddress(const char *name) const override
    {
        return (glw::GenericFuncType)aglGetProcAddress(name);
    }
    void makeCurrent(void) override
    {
        aglMakeCurrent(m_ctx);
    }

private:
    glu::ContextType m_type;
    void *m_ctx;
    glw::Functions m_gl;
    tcu::RenderTarget m_target;
};

static bool fits(int asked, int have)
{
    return asked == glu::RenderConfig::DONT_CARE || asked == have;
}

RenderContext::RenderContext(const glu::RenderConfig &config) : m_type(config.type), m_ctx(nullptr)
{
    const glu::ApiType api = config.type.getAPI();
    if (api != glu::ApiType::es(2, 0) && api != glu::ApiType::es(3, 0) && api != glu::ApiType::es(3, 1))
        throw tcu::NotSupportedError("ogles3.library: OpenGL ES 2.0, 3.0 and 3.1 only");
    if (config.componentType == glu::RenderConfig::COMPONENT_TYPE_FLOAT)
        throw tcu::NotSupportedError("ogles3.library: no floating-point default framebuffer");
    if (!fits(config.redBits, 8) || !fits(config.greenBits, 8) || !fits(config.blueBits, 8) ||
        !fits(config.alphaBits, 8) || !fits(config.depthBits, 24) || !fits(config.stencilBits, 8))
        throw tcu::NotSupportedError("ogles3.library: the default framebuffer is RGBA8, depth 24, stencil 8");
    const int samples = config.numSamples == glu::RenderConfig::DONT_CARE ? 0 : config.numSamples;
    if (samples != 0 && samples != 4)
        throw tcu::NotSupportedError("ogles3.library: 0 or 4 samples");

    const int width  = glu::getValueOrDefault(config, &glu::RenderConfig::width, 256);
    const int height = glu::getValueOrDefault(config, &glu::RenderConfig::height, 256);
    const bool es31  = api == glu::ApiType::es(3, 1);
    const bool es3   = api == glu::ApiType::es(3, 0) || es31;
    ULONG err        = 0;
    m_ctx            = aglCreateContextTags2(
        &err, OGLES3_CCT_PISTORM_WIDTH, (ULONG)width, OGLES3_CCT_PISTORM_HEIGHT, (ULONG)height, OGLES3_CCT_DEPTH,
        24UL, OGLES3_CCT_STENCIL, 8UL, OGLES3_CCT_VSYNC, (ULONG)FALSE, OGLES3_CCT_PISTORM_MSAA,
        (ULONG)(samples ? 4 : 0), OGLES3_CCT_PISTORM_ES_VERSION, es31 ? 31UL : es3 ? 30UL : 20UL, TAG_DONE);
    if (!m_ctx && err == OGLES3_CCE_UNSUPPORTED && es3)
        throw tcu::NotSupportedError(es31 ? "ogles3.library: no OpenGL ES 3.1 contexts in this build"
                                          : "ogles3.library: no OpenGL ES 3.0 contexts in this build");
    if (!m_ctx)
        throw tcu::ResourceError("aglCreateContextTags2 failed, error " + de::toString((unsigned long)err));
    aglMakeCurrent(m_ctx);

    /* The core functions, then those of the extensions GL_EXTENSIONS lists
     * (as the other platforms' glu::initFunctions): a test of an extension's
     * entry points calls them through m_gl (debug_marker). A library before
     * 2.0 ignores the version tag and gives ES 2.0, which GL_VERSION says. */
    FunctionLoader loader(es3);
    if (es3)
    {
        const char *(*getString)(unsigned int) = (const char *(*)(unsigned int))aglGetProcAddress("glGetString");
        const char *version                    = getString ? getString(GL_VERSION) : nullptr;
        /* dEQP-GLES3 runs in a 3.1 context as well: the release library
         * gives one for 3.0 (its newest for 3.x), and a 3.1 context is a 3.0
         * one and more (docs/GLES3_PLAN.md, the remaining gaps) */
        if (!version || (std::string(version).compare(0, 13, "OpenGL ES 3.1") != 0 &&
                         (es31 || std::string(version).compare(0, 13, "OpenGL ES 3.0") != 0)))
        {
            aglDestroyContext(m_ctx);
            m_ctx = nullptr;
            throw tcu::NotSupportedError(es31 ? "ogles3.library: no OpenGL ES 3.1 context (an ES31 build is needed)"
                                              : "ogles3.library: no OpenGL ES 3.0 contexts before 2.0");
        }
        if (es31)
            glw::initES31(&m_gl, &loader);
        else
            glw::initES30(&m_gl, &loader);
    }
    else
        glw::initES20(&m_gl, &loader);
    glu::initExtensionFunctions(&m_gl, &loader, api);
    static bool listed = false;
    if (es3 && !listed && s_numMissing)
    {
        listed = true;
        printf("ogles3.library: %d functions of the ES 3.0 table not implemented yet:", s_numMissing);
        for (int i = 0; i < s_numMissing; i++)
            printf(" %s", s_missing[i]);
        printf("\n");
        fflush(stdout);
    }

    ULONG w = 0, h = 0;
    aglSetParamsTags2(OGLES3_CCT_GET_WIDTH, (ULONG)&w, OGLES3_CCT_GET_HEIGHT, (ULONG)&h, TAG_DONE);
    int r = 0, g = 0, b = 0, a = 0, d = 0, s = 0, n = 0;
    m_gl.getIntegerv(GL_RED_BITS, &r);
    m_gl.getIntegerv(GL_GREEN_BITS, &g);
    m_gl.getIntegerv(GL_BLUE_BITS, &b);
    m_gl.getIntegerv(GL_ALPHA_BITS, &a);
    m_gl.getIntegerv(GL_DEPTH_BITS, &d);
    m_gl.getIntegerv(GL_STENCIL_BITS, &s);
    m_gl.getIntegerv(GL_SAMPLES, &n);
    m_target = tcu::RenderTarget((int)w, (int)h, tcu::PixelFormat(r, g, b, a), d, s, n);
}

RenderContext::~RenderContext(void)
{
    if (m_ctx)
        aglDestroyContext(m_ctx);
}

class ContextFactory : public glu::ContextFactory
{
public:
    ContextFactory(void) : glu::ContextFactory("ogles2", "ogles3.library context in a window")
    {
    }
    glu::RenderContext *createContext(const glu::RenderConfig &config, const tcu::CommandLine &,
                                      const glu::RenderContext *sharedContext) const override
    {
        if (sharedContext)
            throw tcu::NotSupportedError("ogles3.library: shared contexts are not offered to dEQP");
        return new RenderContext(config);
    }
};

class Platform : public tcu::Platform, private glu::Platform
{
public:
    Platform(void)
    {
        if (!OGLES3Open())
            throw tcu::ResourceError("ogles3.library (1.2 or newer) did not open");
        m_contextFactoryRegistry.registerFactory(new ContextFactory());
    }
    ~Platform(void) override
    {
        OGLES3Close();
    }
    bool processEvents(void) override
    {
        return !(SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C);
    }
    const glu::Platform &getGLPlatform(void) const override
    {
        return static_cast<const glu::Platform &>(*this);
    }
    void getMemoryLimits(tcu::PlatformMemoryLimits &limits) const override
    {
        limits.totalSystemMemory = 256 * 1024 * 1024;
    }
};

} // namespace agl
} // namespace tcu

tcu::Platform *createPlatform(void)
{
    return new tcu::agl::Platform();
}
