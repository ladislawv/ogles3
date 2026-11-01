/*
 * ogles3.library: resident tag, initialisation, open/close/expunge and the
 * library vectors (plan section 6.2).
 *
 * Public vector: -30 OGLES3GetDispatchTable(), the only API entry; every GL
 * and agl call goes through the table it returns.
 * Private vector: -36 OGLES3PrivateLogFlush(): writes the log; called by the
 * import library's OGLES3Close() and by the OGLES3Log command, from a process.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "ogles3_internal.h"

#if defined(__GNUC__)
#define REG(reg, type, name) register type name __asm(reg)
#else
#error "register argument syntax for this compiler"
#endif

#define STR2(x) #x
#define STR(x) STR2(x)

static const char LibName[] = "ogles3.library";
static const char LibId[] = "$VER: ogles3.library " STR(OGLES3_LIB_VERSION) "." STR(OGLES3_LIB_REVISION)
                            " (" OGLES3_LIB_DATE ") PiStorm V3D 4.2\r\n";

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *UtilityBase;
struct OGLES3Library *OGLES3LibBase;

extern const struct OGLES3Dispatch ogles2_dispatch_table; /* generated */

static BPTR LibExpunge(REG("a6", struct OGLES3Library *, base));

static void free_base(struct OGLES3Library *base)
{
    FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
}

static void close_system_libraries(void)
{
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = 0;
    }
    if (DOSBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = 0;
    }
}

static struct OGLES3Library *LibInit(REG("d0", struct OGLES3Library *, base),
                                     REG("a0", BPTR, seglist),
                                     REG("a6", struct ExecBase *, sysbase))
{
    SysBase = sysbase;
    base->lib.lib_Node.ln_Type = NT_LIBRARY;
    base->lib.lib_Node.ln_Name = (char *)LibName;
    base->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lib.lib_Version = OGLES3_LIB_VERSION;
    base->lib.lib_Revision = OGLES3_LIB_REVISION;
    base->lib.lib_IdString = (char *)LibId;
    base->segList = seglist;
    InitSemaphore(&base->lock);
    InitSemaphore(&base->compiler_lock);
    OGLES3LibBase = base;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
    UtilityBase = OpenLibrary("utility.library", 37);
    if (!DOSBase || !UtilityBase || !ogles2_log_init()) {
        close_system_libraries();
        free_base(base); /* MakeLibrary allocated it; nobody else will free it */
        return 0;
    }
    ogles2_log("ogles3.library %ld.%ld init", (LONG)OGLES3_LIB_VERSION, (LONG)OGLES3_LIB_REVISION);
    return base;
}

static struct OGLES3Library *LibOpen(REG("a6", struct OGLES3Library *, base))
{
    base->lib.lib_OpenCnt++;
    base->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    ogles2_log("open (count %ld)", (LONG)base->lib.lib_OpenCnt);
    return base;
}

static BPTR LibClose(REG("a6", struct OGLES3Library *, base))
{
    if (base->lib.lib_OpenCnt)
        base->lib.lib_OpenCnt--;
    ogles2_log("close (count %ld)", (LONG)base->lib.lib_OpenCnt);
    /* No log flush here: exec calls Close under Forbid(), where DOS I/O
     * would break the Forbid. OGLES3Close() flushes through -36 first. */
    if (!base->lib.lib_OpenCnt && (base->lib.lib_Flags & LIBF_DELEXP))
        return LibExpunge(base);
    return 0;
}

static BPTR LibExpunge(REG("a6", struct OGLES3Library *, base))
{
    BPTR seg;

    if (base->lib.lib_OpenCnt) {
        base->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seg = base->segList;
    Remove((struct Node *)base);
    ogles2_compiler_stop(); /* exec only: no waiting under Forbid() */
    ogles2_display_libs_close();
    ogles2_log_term();
    close_system_libraries();
    OGLES3LibBase = 0;
    free_base(base);
    return seg;
}

static ULONG LibReserved(void)
{
    return 0;
}

/* -30 */
static const struct OGLES3Dispatch *LibGetDispatchTable(void)
{
    return &ogles2_dispatch_table;
}

/* -36, private: the OGLES3Log command's "write the log now". */
static ULONG LibPrivateLogFlush(void)
{
    return ogles2_log_flush();
}

static const APTR LibVectors[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibReserved,
    (APTR)LibGetDispatchTable,
    (APTR)LibPrivateLogFlush,
    (APTR)-1
};

/* RTF_AUTOINIT: exec's MakeLibrary builds the base from this; LibInit fills
 * the Library fields itself, so there is no data table. */
static const struct {
    ULONG size;
    const APTR *vectors;
    const ULONG *data;
    APTR init;
} InitTable = {
    sizeof(struct OGLES3Library),
    LibVectors,
    0,
    (APTR)LibInit
};

const struct Resident ROMTag = {
    RTC_MATCHWORD,
    (struct Resident *)&ROMTag,
    (APTR)(&ROMTag + 1),
    RTF_AUTOINIT,
    OGLES3_LIB_VERSION,
    NT_LIBRARY,
    0,
    (char *)LibName,
    (char *)LibId,
    (APTR)&InitTable
};
