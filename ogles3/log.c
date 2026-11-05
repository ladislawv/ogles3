/*
 * The library's log (plan section 10): the test machine has no serial
 * capture, so lines are kept in a RAM ring buffer and appended to a file.
 *
 * Writing to the file needs a DOS process outside Forbid(); ogles2_log()
 * itself only touches memory, so it is safe anywhere, including in the
 * library's Open/Close vectors.
 */
#include <stdarg.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "ogles3_internal.h"

#define LOG_SIZE 32768 /* bytes of buffered lines */
#define LINE_MAX 240
#define DEFAULT_PATH "T:ogles3.log"
#define DEFAULT_CAP  (256UL * 1024) /* the default file's: T: is RAM: as a rule */

static char ring[LOG_SIZE];
static ULONG head, used; /* write position, bytes held */
static ULONG lost;       /* lines dropped because the ring was full */
static ULONG seq;        /* line number since the library was loaded */

struct line {
    char text[LINE_MAX + 2];
    ULONG len;
};

/* RawDoFmt's output hook: character in d0, our data in a3. Stops at LINE_MAX. */
static void put_char(register UBYTE c __asm("d0"), register struct line *l __asm("a3"))
{
    if (c && l->len < LINE_MAX)
        l->text[l->len++] = (char)c;
}

/* DOS I/O needs a process. The explicit flush points (OGLES3Close through
 * vector -36, aglDestroyContext, the OGLES3Log command) are reached only from
 * the application's own calls, never under Forbid(), so the process check is
 * all they need. */
/* Held (ogles2_log_hold): no file written meanwhile, the lines wait in
 * the ring. A sync build's write inside a bitmap lock (the job flush's,
 * ogles2_display_lock) restarted the A1200 when the log was on its PFS3
 * disk (dEQP-GLES31 draw_indirect, 2026-10-28). */
static LONG hold;

void ogles2_log_hold(BOOL on)
{
    Forbid();
    hold += on ? 1 : -1;
    Permit();
#ifdef OGLES3_LOG_SYNC
    if (!on && !hold && SysBase->TDNestCnt < 0 && SysBase->IDNestCnt < 0)
        ogles2_log_flush();
#endif
}

static BOOL caller_is_process(void)
{
    struct Task *me = FindTask(0);
    return DOSBase && me->tc_Node.ln_Type == NT_PROCESS;
}

#ifdef OGLES3_LOG_SYNC
/* A line can also be logged inside the library's Open/Close vectors, which
 * exec calls under Forbid(): the per-line flush checks the nest counters
 * (-1 outside Forbid/Disable on AmigaOS; vamos does not maintain them, so
 * this mode writes nothing there). */
static BOOL can_flush_now(void)
{
    return caller_is_process() && SysBase->TDNestCnt < 0 && SysBase->IDNestCnt < 0 && !hold;
}
#endif

/* ogles2_format's output hook: a bounded buffer. */
struct fmt_buf {
    char *p;
    ULONG left; /* room for characters, the NUL excluded */
};

static void put_bounded(register UBYTE c __asm("d0"), register struct fmt_buf *b __asm("a3"))
{
    if (c && b->left) {
        *b->p++ = (char)c;
        b->left--;
    }
}

/* RawDoFmt into buf (size bytes with the NUL); 32-bit arguments as for
 * ogles2_log. Returns the length written. */
/* Probe T: one line per call (n < 0: a step, no count). */
void ogles2_trace(struct OGLES3Context *ctx, const char *what, LONG n, ULONG first)
{
    if (!(ctx->probe & OGLES3_PROBE_TRACE))
        return;
    if (n < 0)
        ogles2_log("trace: %s", (ULONG)what);
    else
        ogles2_log("trace: %s #%lu: %ld names from %lu", (ULONG)what, ++ctx->n_deletes, n, first);
}

ULONG ogles2_format(char *buf, ULONG size, const char *fmt, ...)
{
    struct fmt_buf b;
    va_list ap;
    if (!size)
        return 0;
    b.p = buf;
    b.left = size - 1;
    va_start(ap, fmt);
    RawDoFmt((STRPTR)fmt, (APTR)ap, (void (*)())put_bounded, &b);
    va_end(ap);
    *b.p = 0;
    return (ULONG)(b.p - buf);
}

BOOL ogles2_log_init(void)
{
    head = used = lost = 0;
    return TRUE;
}

void ogles2_log_term(void)
{
    head = used = 0;
}

static void ring_put(const char *s, ULONG n)
{
    ULONG i;
    if (n > LOG_SIZE)
        return;
    if (used + n > LOG_SIZE) {
        lost++;
        return; /* keep the oldest lines: the start of a problem matters most */
    }
    for (i = 0; i < n; i++)
        ring[(head + i) % LOG_SIZE] = s[i];
    head = (head + n) % LOG_SIZE;
    used += n;
}

void ogles2_log(const char *fmt, ...)
{
    struct line l;
    struct line num;
    va_list ap;
    ULONG n;

    va_start(ap, fmt);
    l.len = 0;
    RawDoFmt((STRPTR)fmt, (APTR)ap, (void (*)())put_char, &l);
    va_end(ap);
    l.text[l.len++] = '\n';

    Forbid();
    n = ++seq;
    num.len = 0;
    RawDoFmt((STRPTR) "%6lu ", &n, (void (*)())put_char, &num);
    ring_put(num.text, num.len);
    ring_put(l.text, l.len);
    Permit();

#ifdef OGLES3_LOG_SYNC
    if (can_flush_now())
        ogles2_log_flush();
#endif
}

/* The log file's cap in bytes, 0 for none: ENV:OGLES3/LogMax in KB (0:
 * none), else DEFAULT_CAP for the default file and none for one named in
 * OGLES3/LogFile (the test packages' results, a disk: they want it all).
 * Each context appends its lines at aglDestroyContext, so a program that
 * makes many grew T:'s file, Fast RAM on the RAM disk, without end
 * (2026-11-05, a user's report: 1,040 bytes every cycle or two). */
static ULONG log_cap(BOOL named)
{
    char var[16];
    const char *p;
    ULONG kb = 0;
    if (GetVar((STRPTR) "OGLES3/LogMax", (STRPTR)var, sizeof(var), GVF_GLOBAL_ONLY) <= 0 ||
        var[0] < '0' || var[0] > '9')
        return named ? 0 : DEFAULT_CAP;
    for (p = var; *p >= '0' && *p <= '9' && kb < 0x3FFFFF; p++)
        kb = kb * 10 + (ULONG)(*p - '0');
    return (kb < 0x3FFFFF ? kb : 0x3FFFFF) * 1024; /* within 32 bits */
}

/* Opens the log file to append `n` bytes. One the cap would pass becomes
 * <path>.old (replacing the one before) and a new file starts with a line
 * that says so: the two together stay within about twice the cap. Where the
 * file cannot be renamed it starts over. */
static BPTR log_open(const char *path, ULONG n, ULONG cap)
{
    char old[256 + 4];
    struct line l;
    ULONG k, kb;
    LONG size;
    BOOL renamed;
    BPTR fh = Open((STRPTR)path, MODE_READWRITE);
    if (!fh)
        return 0;
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_CURRENT);
    if (!cap || size <= 0 || (ULONG)size + n <= cap)
        return fh;
    Close(fh);
    for (k = 0; path[k] && k < 256; k++)
        old[k] = path[k];
    CopyMem(".old", old + k, 5);
    DeleteFile((STRPTR)old);
    renamed = Rename((STRPTR)path, (STRPTR)old) != 0;
    if (!(fh = Open((STRPTR)path, MODE_NEWFILE)))
        return 0;
    kb = cap / 1024;
    l.len = 0;
    if (renamed) {
        ULONG args[2];
        args[0] = kb;
        args[1] = (ULONG)old;
        RawDoFmt((STRPTR) "(the log reached its %lu KB, OGLES3/LogMax: the lines before are in %s)\n",
                 args, (void (*)())put_char, &l);
    } else
        RawDoFmt((STRPTR) "(the log reached its %lu KB, OGLES3/LogMax, and starts again)\n", &kb,
                 (void (*)())put_char, &l);
    if (l.len && l.text[l.len - 1] != '\n') /* cut at LINE_MAX */
        l.text[l.len++] = '\n';
    Write(fh, l.text, l.len);
    return fh;
}

BOOL ogles2_log_flush(void)
{
    char path[256];
    char *copy;
    ULONG n, tail, missed;
    BOOL named;
    BPTR fh;

    if (!caller_is_process())
        return FALSE;

    /* Take the lines out under Forbid(), write them outside it. */
    copy = AllocVec(LOG_SIZE + 64, MEMF_ANY);
    if (!copy)
        return FALSE;
    Forbid();
    n = used;
    tail = (head + LOG_SIZE - used) % LOG_SIZE;
    for (missed = 0; missed < n; missed++)
        copy[missed] = ring[(tail + missed) % LOG_SIZE];
    used = 0;
    missed = lost;
    lost = 0;
    Permit();

    if (missed) {
        struct line l;
        l.len = 0;
        RawDoFmt((STRPTR) "(%lu lines lost: log buffer full)\n", &missed, (void (*)())put_char, &l);
        CopyMem(l.text, copy + n, l.len);
        n += l.len;
    }

    if (n) {
        named = GetVar((STRPTR) "OGLES3/LogFile", (STRPTR)path, sizeof(path), GVF_GLOBAL_ONLY) > 0;
        if (!named)
            CopyMem(DEFAULT_PATH, path, sizeof(DEFAULT_PATH));
        fh = log_open(path, n, log_cap(named));
        if (fh) {
            Write(fh, copy, n);
            Close(fh);
        }
    }
    FreeVec(copy);
    return TRUE;
}
