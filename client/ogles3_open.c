/*
 * libogles3.a: opening the library and the per-program-image state
 * (plan section 6.2).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/ogles3.h>

struct Library *OGLES3Base;
const struct OGLES3Dispatch *OGLES3Dispatch;
struct OGLES3Context *OGLES3CurrentContext;

/* lvo.s: the library's vectors */
extern const struct OGLES3Dispatch *OGLES3GetDispatchTableLVO(void);
extern ULONG OGLES3PrivateLogFlushLVO(void);

/* Where the library is looked for: installed (in memory, or LIBS:), then
 * beside the program, so a program can ship it in its own drawer when it is
 * not installed: PROGDIR:libs/, then PROGDIR:. One that is too old, or built
 * for another dispatch ABI, is closed and the next one tried, so a program
 * may also bring a newer one than the system has. */
static const char *const where[] = {
    OGLES3NAME,
    "PROGDIR:libs/" OGLES3NAME,
    "PROGDIR:" OGLES3NAME,
};

/* exec keeps one library of a name: an OpenLibrary of any path returns the
 * ogles3.library in memory (A1200, 2026-10-15). So before the next file is
 * tried, one that would not do is expunged, as Avail FLUSH does, if no
 * program has it open; one in use stays, and the next try gets it again. */
static void expunge_unused(void)
{
    struct Library *l;
    Forbid();
    l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)OGLES3NAME);
    if (l && l->lib_OpenCnt == 0)
        RemLibrary(l);
    Permit();
}

BOOL OGLES3Open(void)
{
    const struct OGLES3Dispatch *d;
    unsigned i;

    if (OGLES3Base && OGLES3Dispatch)
        return TRUE;
    for (i = 0; i < sizeof(where) / sizeof(where[0]); i++) {
        /* OpenLibrary also fails for one in memory older than OGLES3_VERSION */
        if (!OGLES3Base && !(OGLES3Base = OpenLibrary((CONST_STRPTR)where[i], OGLES3_VERSION))) {
            expunge_unused();
            continue;
        }
        d = OGLES3GetDispatchTableLVO();
        /* abiVersion must be EQUAL: a different value means the entries mean
         * something else. structSize "at least mine": a newer library only
         * appends entries (plan section 6.7). */
        if (d && d->abiVersion == OGLES3_DISPATCH_ABI_VERSION && d->structSize >= sizeof(struct OGLES3Dispatch)) {
            OGLES3Dispatch = d;
            return TRUE;
        }
        CloseLibrary(OGLES3Base);
        OGLES3Base = 0;
        expunge_unused();
    }
    return FALSE;
}

void OGLES3Close(void)
{
    OGLES3Dispatch = 0;
    OGLES3CurrentContext = 0;
    if (OGLES3Base) {
        /* Write the library's log while this process may still do DOS I/O
         * (the library's Close vector runs under Forbid). */
        OGLES3PrivateLogFlushLVO();
        CloseLibrary(OGLES3Base);
        OGLES3Base = 0;
    }
}
