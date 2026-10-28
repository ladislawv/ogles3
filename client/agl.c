/*
 * libogles3.a: the agl* functions that maintain the current context, and the
 * varargs forms (plan sections 6.2 and 6.4). The other agl* functions are
 * generated (agl_gen.c).
 */
#include <exec/types.h>
#include <utility/tagitem.h>
#include <proto/ogles3.h>

void aglMakeCurrent(void *context)
{
    if (!OGLES3Dispatch)
        return;
    OGLES3CurrentContext = (struct OGLES3Context *)context;
    OGLES3Dispatch->aglMakeCurrent((struct OGLES3Context *)context);
}

void aglDestroyContext(void *context)
{
    if (!OGLES3Dispatch || !context)
        return;
    if (OGLES3CurrentContext == (struct OGLES3Context *)context)
        OGLES3CurrentContext = 0;
    OGLES3Dispatch->aglDestroyContext((struct OGLES3Context *)context);
}

/* GCC passes all arguments on the stack in order, so the tags after errcode
 * already form a TagItem array (the usual AmigaOS varargs idiom). */
void *aglCreateContextTags2(ULONG *errcode, ...)
{
    return aglCreateContext2(errcode, (struct TagItem *)(&errcode + 1));
}

void aglSetParamsTags2(ULONG tag1, ...)
{
    aglSetParams2((struct TagItem *)&tag1);
}
