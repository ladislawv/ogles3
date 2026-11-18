# dEQP's precision checks on the Pi's free ARM cores

A second build of dEQP-GLES3 and dEQP-GLES31 (`C3=1 MODULE=gles3|gles31
bash ports/deqp/build.sh`, into `$B/deqp-<module>-c3`; the packages
`tests/a1200/m8-2c` and `m8-3c`) whose built-in function precision tests
(`shaders.builtin_functions.precision.*`, dEQP-GLES31's
`builtin_functions.precision.*`) check their values on the Raspberry Pi's
Core 3 and Core 1 as well. It uses the Core 3 SDK 1.4's `core3.library`
(Emu68), `C:\- CPP\Core3_SDK_1.4`. The plain build and its packages (`m7-1`,
`m8-2`, `m8-3`) are left as they are: this one has its own sources, objects
and binary.

Why: those tests spend nearly all their time on the m68k computing each
value's reference interval in doubles (`tcu::Interval`, `tcu::FloatFormat`,
libnix's `ldexp`/`frexp`, the FPU's rounding mode switched per operation;
docs/GLES3_PLAN.md, "dEQP on the Pi's free ARM cores"). The shader runs on
the GPU; only the check moves.

How:
- `patches/c3/0001-precision-checks-on-core3.patch` (applied only for
  `C3=1`) changes `glsBuiltinPrecisionTests.cpp`:
  - `testStatement()`, after the shader ran, hands the case (its function
    group's and its own name, precision, the inputs and outputs, all in Fast
    RAM) to `deqpC3Check()`, and its loop then takes only the values flagged
    there. The log and the result are the same, since the loop logs and
    counts only values outside their range. Without Core 3, or after any
    failure there, the loop takes all of them, as in the plain build.
  - With `DEQP_C3_KERNEL` (the module), each `CaseFactory` gets a
    `c3Check()` that rebuilds the case's statement as `FuncCase` or
    `InOutFuncCase` would. `c3CheckValues()` is the loop without the log.
    The kernels `deqp_c3_setup` (the heap, the ES 3 and ES 3.1 functions)
    and `deqp_c3_check` are at the end of the file.
- `deqp-amiga-c3.c` is the m68k side. It loads `PROGDIR:deqp-c3.c3m` once
  per core: a module's C++ heap is its own, so the two cores never share
  one. It then splits each case's values between Core 3 and Core 1. It
  prints `Core 3: on ...` or `Core 3: off, <why>` once, and a summary at
  the end. Settings in ENV:
  - `DEQP_C3=0` turns it off.
  - `DEQP_C3_CORES=1` uses Core 3 only.
  - `DEQP_C3_HEAP=<MB>` sets each module's heap (default 8).
  - `DEQP_C3_VERIFY=1` (Run-M1 `VERIFY=1`) makes the m68k check every value
    too and log where the two sides differ.
- `deqpC3Kernel.cpp` (module only) holds delibs' case conversions in
  ASCII: theirs use C++ locales.
- `build_module.sh` builds `deqp-c3.c3m` with the Arm GNU Toolchain 15.3.rel2
  (aarch64_be). It compiles the patched source plus `tcuFloatFormat`,
  `tcuInterval` and delibs' maths. Everything else that source refers to
  (the test case, the log, the shader executor, libstdc++'s streams) gets a
  stub: it is never called on Core 3, and if it were, it would stop the
  kernel with its name in `C3Log`. The stubs are found from what the objects
  leave undefined, then by linking. The module is linked with the flags of
  the SDK's `scripts/c3module.sh` and gets its checks (relocations, no
  constructors, no thread-local data, size). The stub list ends up in
  `deqp-c3-stubs.txt` beside it.
  The C library's maths (`sin`, `atan2`, `frexp`, `ldexp`...) is musl
  1.2.5's, fetched into `~/deqp-deps/musl` with a checksum check. glibc's
  libm sets `errno`, which is thread-local data a module cannot have.

## The self-test (on the PC, without the Amiga)

```
bash ports/deqp/c3/selftest/run.sh ~/ogles3-build/m68k-es3/deqp-gles3-c3 [values per case]
```

It runs every case of every function at every precision through the module's
kernel `deqp_c3_selftest`: 705 cases with the ES 3 and ES 3.1 functions. Each
case gets dEQP's own inputs (a fixed seed), with outputs taken from their
reference intervals (midpoints, low ends, high ends), and is then checked. It
runs twice:
- under `qemu-aarch64_be`: `c3run.c` loads the module as core3.library does,
  with its relocations and kernels by name;
- as the same source built for the PC with glibc's libm (`native.cpp`).

Each case's line (values flagged, a hash of the outputs) is compared. A fault
or a stub hit shows as a trap with the stub's name. A difference in the maths
(musl against glibc) shows as a different hash.

`PICKS=256` (pick 0 plus 256) prints every value's inputs and outputs as
32-bit words on both sides, so the two `selftest/*-256.txt` files show which
values differ.
