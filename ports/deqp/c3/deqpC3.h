/* deqpC3.h - dEQP's built-in function precision checks on the Pi's free ARM
 * cores (the Core 3 SDK's core3.library), ports/deqp/c3/README.md.
 *
 * Shared by the m68k (deqp-amiga-c3.c, the patched glsBuiltinPrecisionTests.cpp
 * with DEQP_C3) and the kernels (the same source with DEQP_C3_KERNEL too,
 * built into deqp-c3.c3m): the blocks are 32-bit fields, pointers m68k
 * addresses (C3_PTR on the ARM side).
 */
#ifndef DEQP_C3_H
#define DEQP_C3_H

#include <stdint.h>

#define DEQP_C3_VERSION 1

/* deqp_c3_setup's: the heap of the module's C++ (Fast RAM) */
typedef struct DeqpC3Setup
{
    uint32_t version;  /* DEQP_C3_VERSION */
    uint32_t heap;     /* void * */
    uint32_t heapSize;
    uint32_t reserved;
} DeqpC3Setup;

/* deqp_c3_check's: one case's values */
typedef struct DeqpC3PrecJob
{
    uint32_t version;   /* DEQP_C3_VERSION */
    uint32_t factory;   /* const char *: the function's group (CaseFactory::getName()) */
    uint32_t sub;       /* const char *: the case's name in it (scalar, vec2, mat2x3, highp_vertex...) */
    uint32_t precision; /* glu::Precision */
    uint32_t numValues;
    uint32_t in[4];     /* the inputs, numValues each (0: none) */
    uint32_t out[2];    /* the shader's outputs */
    uint32_t inSize[4]; /* sizeof a value, the m68k's (checked on the ARM side) */
    uint32_t outSize[2];
    uint32_t fmt[5];    /* the case's FloatFormat: min and max exponent, fraction bits, subnormals, infinities */
    uint32_t status;    /* uint8_t[numValues]: 1 for a value outside its range */
    uint32_t first;     /* the values this job checks (deqpC3Check() splits them between the cores) */
    uint32_t count;
    uint32_t result;    /* the kernel's: how many it flagged */
} DeqpC3PrecJob;

/* deqp_c3_selftest's (selftest/README.md): every case of every function at
   every precision, numValues of its inputs, outputs from the reference */
#define DEQP_C3_SELFTEST 0x5e1f7e57u /* DeqpC3PrecJob.version of the self-test's cases */
typedef struct DeqpC3SelfTest
{
    uint32_t version;   /* DEQP_C3_VERSION */
    uint32_t numValues; /* random inputs per case (and the fixed ones) */
    uint32_t pick;      /* the outputs: 0 the references' midpoints, 1 their low ends, 2 their high ends */
    uint32_t cases;     /* the kernel's: cases run, values flagged, a hash of every output */
    uint32_t flagged;
    uint32_t hash;
} DeqpC3SelfTest;

#ifdef DEQP_C3_KERNEL
#include "c3_kernel.h"
#include "c3cxx.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DEQP_C3_KERNEL
/* The m68k side (ports/deqp/c3/deqp-amiga-c3.c). */
/* Nonzero when the checks run on the ARM cores (loaded on first use; off
   after any failure, for the rest of the run). */
int deqpC3Available(void);
/* Fills job->status for every value: the number flagged, or -1 (not done:
   the m68k checks them all). Splits the values between Core 3 and Core 1. */
int deqpC3Check(DeqpC3PrecJob *job);
/* ENV:DEQP_C3_VERIFY=1: the m68k checks every value too and logs where the
   two differ; deqpC3Verified() counts them for the summary at the end. */
int deqpC3Verify(void);
void deqpC3Verified(const char *caseName, int differ);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DEQP_C3_H */
