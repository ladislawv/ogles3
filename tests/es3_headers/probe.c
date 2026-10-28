/*
 * The headers' probe (docs/GLES3_PLAN.md §3): <proto/ogles3.h> with the
 * GLES2 and GLES3 headers included before or after it compiles cleanly, and
 * the ES 3.x entries reach the table. `make es3-header-probe` compiles it
 * once per ORDER with -Werror (GCC 6.5, GCC 16 and vbcc where present):
 *   1 <GLES3/gl3.h> before   2 <GLES3/gl3.h> after   3 <GLES3/gl31.h> after
 *   4 <GLES2/gl2.h>, <GLES3/gl31.h> and <GLES3/gl3ext.h> before
 *   5 <GLES2/gl2.h> and <GLES2/gl2ext.h> before, <GLES3/gl3.h> and
 *     <GLES3/gl3ext.h> after (a GLES2 program that adds ES 3.0)
 * (Khronos' gl2ext.h needs gl2.h or gl3.h before it.)
 */
#if ORDER == 1
#include <GLES3/gl3.h>
#include <proto/ogles3.h>
#elif ORDER == 2
#include <proto/ogles3.h>
#include <GLES3/gl3.h>
#elif ORDER == 3
#include <proto/ogles3.h>
#include <GLES3/gl31.h>
#elif ORDER == 4
#include <GLES2/gl2.h>
#include <GLES3/gl31.h>
#include <GLES3/gl3ext.h>
#include <proto/ogles3.h>
#elif ORDER == 5
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <proto/ogles3.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#else
#error ORDER 1-5
#endif

int probe(void);
int probe(void)
{
    GLint64 v = 0;
    const GLubyte *s = glGetStringi(GL_EXTENSIONS, 0);
    glGetInteger64v(GL_MAX_ELEMENT_INDEX, &v);
    glViewport(0, 0, 1, 1);
    return s != 0 && v > 0 && GL_MAJOR_VERSION == 0x821B && GL_ES_VERSION_3_0 == 1;
}
