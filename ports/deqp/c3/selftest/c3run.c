/* c3run.c - runs deqp-c3.c3m's self-test on the PC (selftest/run.sh): an
 * AArch64 big-endian Linux program, run by qemu-aarch64_be, that loads the
 * module as core3.library does (its PT_LOAD segments, R_AARCH64_RELATIVE
 * relocations, kernels by name from .dynsym) and calls deqp_c3_setup and
 * deqp_c3_selftest with services of its own. Every block the kernels see by
 * a 32-bit address (the m68k's on the Amiga) is mapped below 4 GB.
 *   c3run deqp-c3.c3m <values per case> <pick: 0 mid, 1 low, 2 high>
 */
#include <elf.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "c3_kernel.h"
#include "deqpC3.h"

static void svcLog(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
    fflush(stdout);
}

static uint64_t svcTicks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int svcCancelled(void)
{
    return 0;
}

static void svcClean(const void *addr, size_t n)
{
    (void)addr;
    (void)n;
}

/* memory below 4 GB, as the Amiga's */
static void *low(size_t size)
{
    static uintptr_t next = 0x20000000;
    /* a hint (qemu-user has no MAP_FIXED_NOREPLACE), then checked */
    void *p = mmap((void *)next, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p + size > 0xffffffffu)
    {
        fprintf(stderr, "mmap: no memory below 4 GB at %p (%p)\n", (void *)next, p);
        exit(1);
    }
    next += (size + 0xfffff) & ~(uintptr_t)0xfffff;
    return p;
}

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    unsigned char *file, *base;
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *ph;
    const Elf64_Dyn *dyn = NULL;
    const Elf64_Rela *rela = NULL;
    const Elf64_Sym *syms = NULL;
    const char *strs = NULL;
    const uint32_t *hash = NULL;
    uint64_t span = 0, relasz = 0;
    C3KernelFn setupFn = NULL, testFn = NULL;
    struct C3Services svc;
    DeqpC3Setup *setup;
    DeqpC3SelfTest *test;
    uint32_t r;
    uint64_t t0;
    int i;

    if (argc != 4)
    {
        fprintf(stderr, "c3run deqp-c3.c3m <values per case> <pick>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f)
    {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc(size);
    if (fread(file, 1, size, f) != (size_t)size)
        return 1;
    fclose(f);

    eh = (const Elf64_Ehdr *)file;
    ph = (const Elf64_Phdr *)(file + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > span)
            span = ph[i].p_vaddr + ph[i].p_memsz;
    base = low(span);
    for (i = 0; i < eh->e_phnum; i++)
    {
        if (ph[i].p_type == PT_LOAD)
            memcpy(base + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_type == PT_DYNAMIC)
            dyn = (const Elf64_Dyn *)(base + ph[i].p_vaddr);
    }
    for (; dyn && dyn->d_tag != DT_NULL; dyn++)
        switch (dyn->d_tag)
        {
        case DT_RELA: rela = (const Elf64_Rela *)(base + dyn->d_un.d_ptr); break;
        case DT_RELASZ: relasz = dyn->d_un.d_val; break;
        case DT_SYMTAB: syms = (const Elf64_Sym *)(base + dyn->d_un.d_ptr); break;
        case DT_STRTAB: strs = (const char *)(base + dyn->d_un.d_ptr); break;
        case DT_HASH: hash = (const uint32_t *)(base + dyn->d_un.d_ptr); break;
        }
    for (i = 0; rela && i < (int)(relasz / sizeof(Elf64_Rela)); i++)
    {
        if (ELF64_R_TYPE(rela[i].r_info) != R_AARCH64_RELATIVE)
        {
            fprintf(stderr, "relocation %lu\n", (unsigned long)ELF64_R_TYPE(rela[i].r_info));
            return 1;
        }
        *(uint64_t *)(base + rela[i].r_offset) = (uint64_t)(uintptr_t)base + (uint64_t)rela[i].r_addend;
    }
    for (i = 0; hash && syms && strs && i < (int)hash[1]; i++)
    {
        if (!strcmp(strs + syms[i].st_name, "deqp_c3_setup"))
            setupFn = (C3KernelFn)(void *)(base + syms[i].st_value);
        if (!strcmp(strs + syms[i].st_name, "deqp_c3_selftest"))
            testFn = (C3KernelFn)(void *)(base + syms[i].st_value);
    }
    if (!setupFn || !testFn)
    {
        fprintf(stderr, "%s: no deqp_c3_setup or deqp_c3_selftest\n", argv[1]);
        return 1;
    }
    __builtin___clear_cache((char *)base, (char *)base + span);

    memset(&svc, 0, sizeof(svc));
    svc.version      = C3_SERVICES_VERSION;
    svc.size         = sizeof(svc);
    svc.flags        = C3SVC_NATIVE;
    svc.cntfrq       = 1000000000;
    svc.log          = svcLog;
    svc.ticks        = svcTicks;
    svc.memcpy       = memcpy;
    svc.memset       = memset;
    svc.cancelled    = svcCancelled;
    svc.dcache_clean = svcClean;

    setup           = low(4096);
    setup->version  = DEQP_C3_VERSION;
    setup->heapSize = 16 << 20;
    setup->heap     = (uint32_t)(uintptr_t)low(setup->heapSize);
    r               = setupFn((uint32_t)(uintptr_t)setup, &svc);
    if (r != 1)
    {
        fprintf(stderr, "deqp_c3_setup: %u\n", r);
        return 1;
    }
    test            = low(4096);
    test->version   = DEQP_C3_VERSION;
    test->numValues = (uint32_t)atoi(argv[2]);
    test->pick      = (uint32_t)atoi(argv[3]);
    t0              = svcTicks();
    r               = testFn((uint32_t)(uintptr_t)test, &svc);
    printf("selftest: %u, %u cases, %u flagged, hash %08x (%.1f s under qemu)\n", r, test->cases, test->flagged,
           test->hash, (double)(svcTicks() - t0) / 1e9);
    return r == 1 ? 0 : 1;
}
