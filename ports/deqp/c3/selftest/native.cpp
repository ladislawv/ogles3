// native.cpp - the self-test's kernels built for the PC (selftest/run.sh):
// the same patched glsBuiltinPrecisionTests.cpp with DEQP_C3_KERNEL, glibc's
// libm instead of musl's, to compare with the module's lines under qemu. A
// non-PIE program, so its static blocks have 32-bit addresses as the
// kernels take them; what the precision code refers to and never calls is
// left unresolved.
#include <cstdio>
#include <cstdlib>

#include "deqpC3.h"

extern "C" uint32_t deqp_c3_setup(uint32_t arg, const C3Services *svc);
extern "C" uint32_t deqp_c3_selftest(uint32_t arg, const C3Services *svc);

namespace c3cxx
{
void init(const C3Services *, void *, size_t)
{
}
void set_services(const C3Services *)
{
}
[[noreturn]] void fatal(const char *what)
{
    std::fprintf(stderr, "fatal: %s\n", what);
    std::abort();
}
} // namespace c3cxx

static DeqpC3Setup setup;
static DeqpC3SelfTest test;

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "native <values per case> <pick>\n");
        return 2;
    }
    setup.version = DEQP_C3_VERSION;
    if (deqp_c3_setup((uint32_t)(uintptr_t)&setup, nullptr) != 1)
        return 1;
    test.version   = DEQP_C3_VERSION;
    test.numValues = (uint32_t)std::atoi(argv[1]);
    test.pick      = (uint32_t)std::atoi(argv[2]);
    const uint32_t r = deqp_c3_selftest((uint32_t)(uintptr_t)&test, nullptr);
    std::printf("selftest: %u, %u cases, %u flagged, hash %08x (native)\n", r, test.cases, test.flagged, test.hash);
    return r == 1 ? 0 : 1;
}
