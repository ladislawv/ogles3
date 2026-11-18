/* deqp-amiga-c3.c - the m68k side of dEQP's built-in function precision
 * checks on the Pi's free ARM cores (ports/deqp/c3/README.md): loads
 * PROGDIR:deqp-c3.c3m through core3.library once per core (a module's C++
 * heap is its own, so Core 3 and Core 1 never share one), and splits each
 * case's values between them. Off for the rest of the run after any
 * failure: the m68k then checks every value, as the plain dEQP does.
 *
 * ENV: DEQP_C3=0 off; DEQP_C3_CORES=1 Core 3 only; DEQP_C3_HEAP=<MB> each
 * module's heap (8); DEQP_C3_VERIFY=1 the m68k checks every value too and
 * logs where the two differ.
 */
#include <exec/memory.h>
#include <dos/var.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core3.h"
#include "deqpC3.h"

struct Library *Core3Base;

#define MAX_CORES 2
#define NAME_MAX_ 120

/* a job block in Fast RAM, with the names copied beside it */
typedef struct Block
{
    DeqpC3PrecJob job;
    char factory[NAME_MAX_];
    char sub[NAME_MAX_];
} Block;

static int state; /* 0: not tried yet, 1: on, -1: off */
static int verify = -1;
static int ncores;
static struct C3Module *mods[MAX_CORES];
static ULONG checkEntry[MAX_CORES];
static APTR heaps[MAX_CORES];
static Block *blocks[MAX_CORES];
static ULONG cntfrq;
static unsigned long statCases, statValues, statFlagged, statOther;
static double statSecs[MAX_CORES];
static unsigned long verifyCases, verifyDiffer;

/* ENV:name, or NULL (libnix's getenv reads nothing here: as
   patches/0003-amiga-progress-file.patch) */
static const char *env(const char *name)
{
    static char buf[4][16];
    static int next;
    char *b = buf[next++ & 3];

    return GetVar((STRPTR)name, (STRPTR)b, sizeof(buf[0]), GVF_GLOBAL_ONLY) > 0 ? b : NULL;
}

static const char *errName(ULONG err)
{
    static const char *const names[] = {"OK",  "NOC3",  "NOFILE",   "NOMEM", "FORMAT",    "RELOC",    "ICACHE",
                                        "JOB", "TIMEOUT", "LOST",   "ARG",   "FAULT",     "WATCHDOG", "CANCELLED"};
    return err < sizeof(names) / sizeof(names[0]) ? names[err] : "?";
}

static void summary(void)
{
    if (statCases)
        printf("Core 3: %lu cases, %lu values checked there (%lu flagged), %lu cases on the m68k; "
               "%.1f s on Core 3, %.1f s on Core 1\n",
               statCases, statValues, statFlagged, statOther, statSecs[0], statSecs[1]);
    if (verifyCases)
        printf("Core 3 verify: %lu cases, %lu values differ\n", verifyCases, verifyDiffer);
    fflush(stdout);
}

static void off(const char *why, ULONG err)
{
    if (err)
        printf("Core 3: off, %s (C3ERR_%s)\n", why, errName(err));
    else
        printf("Core 3: off, %s\n", why);
    fflush(stdout);
    state = -1;
}

/* A ticket's end: C3ERR_*, the result in *res. Waits up to 3 minutes, then
   cancels it (and waits for that). */
static ULONG finish(ULONG ticket, struct C3Result *res)
{
    ULONG err = C3ERR_TIMEOUT;
    int i;

    for (i = 0; i < 3 && err == C3ERR_TIMEOUT; i++)
        err = C3_WaitResult(ticket, res, 60);
    if (err == C3ERR_TIMEOUT)
    {
        C3_Cancel(ticket);
        C3_WaitResult(ticket, res, 10);
    }
    return err;
}

static void start(void)
{
    const char *e = env("DEQP_C3");
    struct C3Info info;
    int want = MAX_CORES, mb = 8, i;

    state = -1;
    if (e && e[0] == '0')
    {
        off("DEQP_C3=0", 0);
        return;
    }
    Core3Base = OpenLibrary((CONST_STRPTR)CORE3NAME, CORE3_VERSION);
    if (!Core3Base)
    {
        off("no core3.library", 0);
        return;
    }
    memset(&info, 0, sizeof(info));
    if (!C3_GetInfo(&info))
    {
        off(info.forced ? "ENV:C3Force68k" : "no Core 3 service", 0);
        return;
    }
    cntfrq = info.cntfrq ? info.cntfrq : 1;
    if (!(info.features & C3FEAT_CORE1) || Core3Base->lib_Revision < 4)
        want = 1;
    e = env("DEQP_C3_CORES");
    if (e && e[0] == '1')
        want = 1;
    e = env("DEQP_C3_HEAP");
    if (e && atoi(e) > 0)
        mb = atoi(e);

    for (i = 0; i < want; i++)
    {
        struct C3Result res;
        DeqpC3Setup *setup;
        ULONG err = 0, ticket, setupEntry;

        mods[i] = C3_LoadModule((CONST_STRPTR) "PROGDIR:deqp-c3.c3m", &err);
        if (!mods[i])
        {
            off("PROGDIR:deqp-c3.c3m not loaded", err);
            return;
        }
        setupEntry    = C3_FindSymbol(mods[i], (CONST_STRPTR) "deqp_c3_setup");
        checkEntry[i] = C3_FindSymbol(mods[i], (CONST_STRPTR) "deqp_c3_check");
        heaps[i]      = C3_AllocShared((ULONG)mb << 20);
        blocks[i]     = (Block *)C3_AllocShared(sizeof(Block));
        setup         = (DeqpC3Setup *)C3_AllocShared(sizeof(DeqpC3Setup));
        if (!setupEntry || !checkEntry[i])
        {
            off("no kernels in deqp-c3.c3m", 0);
            return;
        }
        if (!heaps[i] || !blocks[i] || !setup)
        {
            off("no Fast RAM for the heap", 0);
            return;
        }
        setup->version  = DEQP_C3_VERSION;
        setup->heap     = (uint32_t)heaps[i];
        setup->heapSize = (uint32_t)mb << 20;
        ticket          = C3_SubmitEx(setupEntry, (ULONG)setup, i ? C3F_CORE1 : 0, 60000);
        err             = ticket ? finish(ticket, &res) : C3ERR_NOC3;
        C3_FreeShared(setup);
        if (err != C3ERR_OK || res.result != 1)
        {
            if (err != C3ERR_OK)
                printf("Core 3: deqp_c3_setup on %s: state %lu, pc 0x%08lx, address 0x%08lx, syndrome 0x%08lx "
                       "(C3Log says more)\n",
                       i ? "Core 1" : "Core 3", (unsigned long)res.state, (unsigned long)res.fault_pc,
                       (unsigned long)res.result2, (unsigned long)res.result);
            off(err != C3ERR_OK ? "deqp_c3_setup failed" : "deqp_c3_setup refused (another version?)", err);
            return;
        }
    }
    ncores = want;
    state  = 1;
    atexit(summary);
    printf("Core 3: on, the precision checks on %s (%d MB heap each)\n", ncores > 1 ? "Core 3 and Core 1" : "Core 3",
           mb);
    fflush(stdout);
}

int deqpC3Available(void)
{
    if (state == 0)
        start();
    return state > 0;
}

int deqpC3Verify(void)
{
    if (verify < 0)
    {
        const char *e = env("DEQP_C3_VERIFY");
        verify        = e && e[0] == '1';
    }
    return verify;
}

void deqpC3Verified(const char *caseName, int differ)
{
    verifyCases++;
    verifyDiffer += (unsigned long)differ;
    if (differ)
        printf("Core 3 verify: %s: %d values differ\n", caseName, differ);
}

/* the whole of [p, p + n) in Fast RAM (the ARM cores never touch Chip RAM) */
static int fast(uint32_t p, uint32_t n)
{
    if (!p || !n)
        return 1;
    return (TypeOfMem((APTR)p) & MEMF_FAST) && (TypeOfMem((APTR)(p + n - 1)) & MEMF_FAST);
}

int deqpC3Check(DeqpC3PrecJob *job)
{
    const char *factory = (const char *)job->factory, *sub = (const char *)job->sub;
    ULONG tickets[MAX_CORES];
    uint32_t n = job->numValues, first = 0;
    int i, flagged = 0, failed = 0;

    if (state <= 0 || !n)
        return -1;
    if (strlen(factory) >= NAME_MAX_ || strlen(sub) >= NAME_MAX_ || !fast(job->status, n) ||
        !fast(job->in[0], n * job->inSize[0]) || !fast(job->in[1], n * job->inSize[1]) ||
        !fast(job->in[2], n * job->inSize[2]) || !fast(job->in[3], n * job->inSize[3]) ||
        !fast(job->out[0], n * job->outSize[0]) || !fast(job->out[1], n * job->outSize[1]))
    {
        statOther++;
        return -1;
    }

    for (i = 0; i < ncores; i++)
    {
        Block *b = blocks[i];
        uint32_t count = i == ncores - 1 ? n - first : n / ncores;

        b->job = *job;
        strcpy(b->factory, factory);
        strcpy(b->sub, sub);
        b->job.factory = (uint32_t)b->factory;
        b->job.sub     = (uint32_t)b->sub;
        b->job.first   = first;
        b->job.count   = count;
        b->job.result  = 0;
        first += count;
        tickets[i] = C3_SubmitEx(checkEntry[i], (ULONG)&b->job, i ? C3F_CORE1 : 0, 170000);
    }
    for (i = 0; i < ncores; i++)
    {
        struct C3Result res;
        ULONG err;

        memset(&res, 0, sizeof(res));
        err = tickets[i] ? finish(tickets[i], &res) : C3ERR_NOC3;
        if (err != C3ERR_OK)
        {
            if (!failed)
            {
                printf("Core 3: %s: %s on %s, state %lu, pc 0x%08lx, address 0x%08lx, syndrome 0x%08lx\n", factory,
                       sub, i ? "Core 1" : "Core 3", (unsigned long)res.state, (unsigned long)res.fault_pc,
                       (unsigned long)res.result2, (unsigned long)res.result);
                off("a check failed", err);
            }
            failed = 1;
            continue;
        }
        if (res.result != 1)
        {
            failed = 1; /* this case is not there (another version?): the m68k's */
            continue;
        }
        statSecs[i] += (double)res.ticks / (double)cntfrq;
        flagged += (int)blocks[i]->job.result;
    }
    if (failed)
    {
        statOther++;
        return -1;
    }
    statCases++;
    statValues += n;
    statFlagged += (unsigned long)flagged;
    return flagged;
}
