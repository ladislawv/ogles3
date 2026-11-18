#!/bin/bash
# Builds deqp-c3.c3m, the Core 3 module of dEQP's built-in function
# precision checks (README.md here), from the patched sources of a C3=1
# build (../build.sh calls it):
#   bash build_module.sh <the build's src/> <out.c3m>
# The kernels need only the precision code (glsBuiltinPrecisionTests.cpp
# with DEQP_C3_KERNEL), tcu::FloatFormat and tcu::Interval, delibs' maths:
# the rest of what that source refers to (the test case, the log, the shader
# executor, libstdc++'s streams for printing) is never called there, and
# gets a stub that stops the kernel with its name in C3Log. The stubs are
# what the objects leave undefined, then by linking until it links; the
# module is linked as the SDK's scripts/c3module.sh links one, and gets its
# checks. The C library's maths (sin, frexp...) is musl's (fetched into
# $DEPS like ../build.sh's zlib): glibc's libm sets errno, thread-local data
# a module cannot have. Needs the Arm GNU Toolchain 15.3.rel2
# (aarch64_be-none-linux-gnu: ARM_GNU_TOOLCHAIN, the SDK's default place).
set -eu
ROOT=${1:?the src directory of the build}
OUTM=${2:?the module to write}
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${C3_SDK_LINK:-$HOME/.core3-sdk}
ARM_GNU=${ARM_GNU_TOOLCHAIN:-$HOME/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu}
CC=$ARM_GNU/bin/aarch64_be-none-linux-gnu-gcc
CXX=$ARM_GNU/bin/aarch64_be-none-linux-gnu-g++
FILT=$ARM_GNU/bin/aarch64_be-none-linux-gnu-c++filt
[ -x "$CXX" ] || { echo "no Arm GNU Toolchain in $ARM_GNU (ARM_GNU_TOOLCHAIN=...)"; exit 1; }
[ -f "$SDK/scripts/c3module.sh" ] || { echo "no Core 3 SDK at $SDK"; exit 1; }

DEPS=${DEPS:-$HOME/deqp-deps}
MUSL_URL=https://musl.libc.org/releases/musl-1.2.5.tar.gz
MUSL_SHA=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
M=$DEPS/musl
if [ ! -d "$M" ]; then
    mkdir -p "$DEPS/dl"
    a=$DEPS/dl/musl-1.2.5.tar.gz
    [ -f "$a" ] || curl -sSL -o "$a" "$MUSL_URL" || { echo "could not download $MUSL_URL"; exit 1; }
    echo "$MUSL_SHA  $a" | sha256sum -c --quiet - || { echo "$a: wrong checksum"; rm -f "$a"; exit 1; }
    mkdir -p "$M.tmp"
    tar -xzf "$a" -C "$M.tmp" --strip-components=1
    mv "$M.tmp" "$M"
fi
# what dEQP's maths calls (nm of the objects), and what that needs
MUSL_SRCS=(
    math/sin.c math/cos.c math/__sin.c math/__cos.c math/__rem_pio2.c math/__rem_pio2_large.c
    math/acos.c math/asin.c math/atan.c math/atan2.c math/exp.c math/exp_data.c math/log.c math/log_data.c
    math/pow.c math/pow_data.c math/frexp.c math/ldexp.c math/ldexpf.c math/scalbn.c math/scalbnf.c
    math/modf.c math/aarch64/floor.c math/aarch64/fabs.c math/aarch64/sqrt.c
    string/strdup.c
)

F=$ROOT/framework
D=$F/delibs
# not ../src/include (libnix's fenv.h): the kernel's rounding modes are glibc's libm's
INC="-I$D/debase -I$D/depool -I$D/dethread -I$D/destream -I$D/deutil -I$D/decpp -I$F/qphelper
-I$F/common -I$F/opengl -I$F/opengl/wrapper -I$F/referencerenderer -I$ROOT/modules/glshared
-I$ROOT/amiga/c3"
DEFS="-DNDEBUG -DDEQP_C3 -DDEQP_C3_KERNEL"
SRCS=(
    modules/glshared/glsBuiltinPrecisionTests.cpp
    framework/common/tcuFloatFormat.cpp framework/common/tcuInterval.cpp framework/common/tcuFloat.cpp
    amiga/c3/deqpC3Kernel.cpp framework/delibs/decpp/deRandom.cpp
    framework/opengl/gluShaderUtil.cpp framework/opengl/gluVarType.cpp
    framework/delibs/debase/deMath.c framework/delibs/debase/deInt32.c framework/delibs/debase/deFloat16.c
    framework/delibs/debase/deRandom.c framework/delibs/debase/deMemory.c framework/delibs/debase/deString.c
)

W=${KEEP_DIR:-$(mktemp -d)}; mkdir -p "$W"
[ -n "${KEEP_DIR:-}" ] || trap 'rm -rf "$W"' EXIT
# c3module.sh's flags (SDK 1.4), C++17 for dEQP rather than its C++20
ARCH="-march=armv8-a -mtune=cortex-a72 -mbig-endian -ffp-contract=off -fno-math-errno"
COMMON="-O2 $ARCH -fPIC -fvisibility=hidden -fno-plt -fno-common -mno-outline-atomics
        -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -w"
LDFLAGS="-shared -nostdlib -nostartfiles
         -Wl,-Bsymbolic,--hash-style=sysv,-z,max-page-size=4096,-z,norelro,-z,noexecstack,--no-undefined,--build-id=none
         -Wl,--gc-sections,--exclude-libs,ALL,--no-demangle"
LIBS="-L$ARM_GNU/aarch64_be-none-linux-gnu/lib64 -L$ARM_GNU/aarch64_be-none-linux-gnu/libc/usr/lib64"
NM=$ARM_GNU/bin/aarch64_be-none-linux-gnu-nm
READELF=$ARM_GNU/bin/aarch64_be-none-linux-gnu-readelf
# musl's own build: its headers only, its bits/alltypes.h made as its Makefile makes it
mkdir -p "$W/musl/bits"
sed -f "$M/tools/mkalltypes.sed" "$M/arch/aarch64/bits/alltypes.h.in" "$M/include/alltypes.h.in" > "$W/musl/bits/alltypes.h"
MUSLFLAGS="-std=c99 -nostdinc -ffreestanding -D_XOPEN_SOURCE=700 -frounding-math -fexcess-precision=standard
           -I$M/arch/aarch64 -I$M/arch/generic -I$M/src/include -I$M/src/internal -I$W/musl -I$M/include"

compile() { # source object
    case $1 in
    musl/*) "$CC" $MUSLFLAGS $COMMON -c "$M/src/${1#musl/}" -o "$2" ;;
    *.c) "$CC" -std=gnu11 -ffreestanding $COMMON -I$SDK/include -I$SDK/runtime $INC $DEFS -c "$ROOT/$1" -o "$2" ;;
    *) "$CXX" -std=gnu++17 -fno-threadsafe-statics $COMMON -I$SDK/include -I$SDK/runtime $INC $DEFS -c "$ROOT/$1" -o "$2" ;;
    esac
}
objs=()
n=0
for s in "${SRCS[@]}" "${MUSL_SRCS[@]/#/musl/}"; do
    o=$W/$n.o
    n=$((n + 1))
    compile "$s" "$o" &
    objs+=("$o")
done
"$CC" -std=gnu11 -ffreestanding $COMMON -I$SDK/include -c "$SDK/runtime/c3rt.c" -o "$W/rt.o" &
"$CXX" -std=c++20 -fno-threadsafe-statics $COMMON -I$SDK/include -I$SDK/runtime -c "$SDK/runtime/c3cxx.cpp" -o "$W/cxx.o" &
for j in $(jobs -p); do wait "$j" || { echo "build_module: a compile failed"; exit 1; }; done
"$ARM_GNU/bin/aarch64_be-none-linux-gnu-ar" rcs "$W/librt.a" "$W/rt.o" "$W/cxx.o"

undefined() { # the link's output: the undefined symbols, mangled
    sed -n "s/.*undefined reference to \`\(.*\)'.*/\1/p" "$1" | sort -u
}
# libstdc++'s printing (the precision code prints its expressions and values
# into the log, never on Core 3; their pieces want glibc's locales and
# stdio) and its exception handling (the SDK's runtime has its own: a throw
# stops the kernel)
STREAMS='ostream|istream|stringstream|stringbuf|streambuf|ios_base|basic_ios|_Ios_|locale|num_put|num_get|ctype|cout|cerr|clog|basic_file|filebuf|fstream|codecvt|numpunct|moneypunct|money_|messages|collate|time_get|time_put|__facet|random_device'
EH='uncaught_exception|terminate|unexpected|exception_ptr|current_exception|nested_exception|std::exception|runtime_error|logic_error|out_of_range|length_error|invalid_argument|domain_error|range_error|overflow_error|underflow_error|bad_alloc|bad_cast|bad_typeid|bad_array|system_error|error_category'

# 1: what the objects leave undefined (before --gc-sections, as the linker
# picks archive members): dEQP's own symbols, and libstdc++'s streams, are
# stubbed; the rest (strings, maps...) comes from libstdc++.
for o in "${objs[@]}" "$W/rt.o" "$W/cxx.o"; do "$NM" -u "$o"; done | awk '{print $2}' | sort -u > "$W/u-all.txt"
for o in "${objs[@]}" "$W/rt.o" "$W/cxx.o"; do "$NM" --defined-only "$o"; done | awk '{print $3}' | sort -u > "$W/d-all.txt"
comm -23 "$W/u-all.txt" "$W/d-all.txt" > "$W/u.txt"
: > "$W/stubs.txt"
while read -r sym; do
    dem=$(echo "$sym" | "$FILT")
    case $sym in
    _Z*)
        if ! echo "$dem" | grep -qE "^(std::|__gnu_cxx::|__cxxabiv1::|operator (new|delete)|(vtable|typeinfo|typeinfo name|VTT|construction vtable) for (std::|__gnu_cxx::|__cxxabiv1::))" ||
            echo "$dem" | grep -qE "$STREAMS|$EH"; then
            echo "$sym" >> "$W/stubs.txt"
        fi ;;
    de* | qp*) echo "$sym" >> "$W/stubs.txt" ;; # delibs' C we do not build
    esac
done < "$W/u.txt"

gen_stubs() { # stubs.txt -> stubs.o
    {
        echo '// generated by ports/deqp/c3/build_module.sh: never called on Core 3'
        echo '#include "c3cxx.h"'
        echo 'extern "C" __attribute__((visibility("hidden"), noreturn, used)) void deqp_c3_stub_hit(const char *name)'
        echo '{'
        echo '    c3cxx::fatal(name);'
        echo '}'
        local i=0 sym dem
        while read -r sym; do
            [ -n "$sym" ] || continue
            dem=$(echo "$sym" | "$FILT" | tr -d '"\\' | cut -c1-200)
            printf '__asm__(".section .text.deqp_c3_stub_%d,\\"ax\\",%%progbits\\n.globl %s\\n.hidden %s\\n.type %s, %%function\\n.p2align 2\\n%s:\\nadr x0, 1f\\nb deqp_c3_stub_hit\\n1: .asciz \\"stub: %s\\"\\n.p2align 2\\n.previous\\n");\n' \
                "$i" "$sym" "$sym" "$sym" "$sym" "$dem"
            i=$((i + 1))
        done < "$W/stubs.txt"
    } > "$W/stubs.cpp"
    "$CXX" -std=c++20 $COMMON -I$SDK/runtime -c "$W/stubs.cpp" -o "$W/stubs.o"
}

# 2..: with the libraries; what is still undefined (what libstdc++'s
# members want: glibc), until it links. -lm: fegetround, fesetround.
OUTDIR=$(dirname "$OUTM")
for pass in 1 2 3 4 5 6; do
    gen_stubs
    if "$CXX" -mbig-endian $LDFLAGS -Wl,-Map="$W/module.map" -o "$W/m.c3m" "${objs[@]}" "$W/stubs.o" $LIBS \
        -Wl,-Bstatic,--start-group "$W/librt.a" -lstdc++ -lm -lgcc -Wl,--end-group > "$W/link.txt" 2>&1; then
        break
    fi
    if grep -q "multiple definition" "$W/link.txt"; then
        # a member of libstdc++ that has no place here (its streams, its
        # exception handling): why the linker took it
        grep "multiple definition" "$W/link.txt" | head -5
        awk '/^Archive member included/{on=1} /^Discarded|^Memory map|^Allocating common/{on=0} on' "$W/module.map" |
            paste - - | sed 's/  */ /g' | "$FILT" | cut -c1-300 | tail -60
        echo "build_module: a member of libstdc++ to keep out (STREAMS, EH above)"
        exit 1
    fi
    undefined "$W/link.txt" >> "$W/stubs.txt"
    sort -u "$W/stubs.txt" -o "$W/stubs.txt"
    [ "$pass" = 6 ] && { head -40 "$W/link.txt"; echo "build_module: does not link"; exit 1; }
done
"$FILT" < "$W/stubs.txt" > "$OUTDIR/deqp-c3-stubs.txt"
echo "stubs: $(wc -l < "$W/stubs.txt") ($OUTDIR/deqp-c3-stubs.txt); C names among them:"
grep -v "^_Z" "$W/stubs.txt" | tr '\n' ' '
echo

# scripts/c3module.sh's checks (SDK 1.4): what core3.library's loader accepts
out=$W/m.c3m
fail() { echo "build_module: $*"; exit 1; }
"$READELF" -h "$out" | grep -q "big endian" || fail "not big-endian"
"$READELF" -h "$out" | grep -q "DYN" || fail "not ET_DYN"
! "$READELF" -d "$out" | grep -qE "NEEDED|TEXTREL" || fail "needs a shared library, or has TEXTREL"
bad=$("$READELF" -rW "$out" | awk '/^[0-9a-f]+ +[0-9a-f]+ +R_/ && $3 != "R_AARCH64_RELATIVE"' | head -5)
[ -z "$bad" ] || fail "relocations other than R_AARCH64_RELATIVE:
$bad"
! "$READELF" -SW "$out" | grep -qE "\.(tbss|tdata|ctors|preinit_array)" || fail "thread-local data or constructors"
ctors=$(awk '/^\.init_array/{on=1;next} on && /^\.[a-z]/{on=0} on && /\.o\)?$/' "$W/module.map" | grep -v "lse-init.o" || true)
[ -z "$ctors" ] || fail "global constructors (never run on Core 3):
$ctors"
size=$(stat -c %s "$out")
[ "$size" -le $((16 << 20)) ] || fail "larger than 16 MB (core3.library's limit)"
exports=$("$READELF" -W --dyn-syms "$out" | awk '$4 == "FUNC" && $5 == "GLOBAL" && $7 != "UND" {printf "%s ", $8}')
for k in deqp_c3_setup deqp_c3_check; do
    case " $exports" in *" $k "*) ;; *) fail "does not export $k" ;; esac
done
cp "$out" "$OUTM"
echo "$OUTM: $size bytes"
