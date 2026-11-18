#!/bin/bash
# deqp-c3.c3m's self-test on the PC (../README.md, "The self-test"): every
# case of every function at every precision through the module's kernels,
# under qemu-aarch64_be (c3run.c loads it as core3.library does), and
# through the same code built for the PC with glibc's libm (native.cpp), on
# the same inputs. Each case's line (values flagged, a hash of the outputs
# taken from the reference intervals: their midpoints, low and high ends)
# must be the same in both: the module neither faults nor hits a stub, and
# its maths (musl's) gives the references glibc's does.
#   bash ports/deqp/c3/selftest/run.sh <a C3=1 build: $B/deqp-gles3-c3> [values per case (64)]
set -eu
D=${1:?a C3=1 build directory}
N=${2:-64}
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${C3_SDK_LINK:-$HOME/.core3-sdk}
ARM_GNU=${ARM_GNU_TOOLCHAIN:-$HOME/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu}
QEMU=${QEMU:-qemu-aarch64_be}
ROOT=$D/src
OUT=$D/selftest
[ -f "$D/deqp-c3.c3m" ] || { echo "no $D/deqp-c3.c3m (C3=1 ports/deqp/build.sh)"; exit 1; }
command -v "$QEMU" > /dev/null || { echo "no $QEMU (apt install qemu-user)"; exit 1; }
mkdir -p "$OUT"

"$ARM_GNU/bin/aarch64_be-none-linux-gnu-gcc" -O2 -static-pie -fPIE -I"$SDK/include" -I"$ROOT/amiga/c3" -o "$OUT/c3run" "$HERE/c3run.c"

F=$ROOT/framework
DL=$F/delibs
INC="-I$DL/debase -I$DL/depool -I$DL/dethread -I$DL/destream -I$DL/deutil -I$DL/decpp -I$F/qphelper
-I$F/common -I$F/opengl -I$F/opengl/wrapper -I$F/referencerenderer -I$ROOT/modules/glshared
-I$ROOT/amiga/c3 -I$SDK/include -I$SDK/runtime"
DEFS="-DNDEBUG -DDEQP_C3 -DDEQP_C3_KERNEL"
SRCS=(
    modules/glshared/glsBuiltinPrecisionTests.cpp
    framework/common/tcuFloatFormat.cpp framework/common/tcuInterval.cpp framework/common/tcuFloat.cpp
    framework/delibs/decpp/deStringUtil.cpp framework/delibs/decpp/deRandom.cpp
    framework/opengl/gluShaderUtil.cpp framework/opengl/gluVarType.cpp
    framework/delibs/debase/deMath.c framework/delibs/debase/deInt32.c framework/delibs/debase/deFloat16.c
    framework/delibs/debase/deRandom.c framework/delibs/debase/deMemory.c framework/delibs/debase/deString.c
)
objs=()
n=0
for s in "${SRCS[@]}"; do
    o=$OUT/n$n.o
    n=$((n + 1))
    case $s in
    *.c) gcc -std=gnu11 -O2 -ffp-contract=off -fno-pie -w $INC $DEFS -c "$ROOT/$s" -o "$o" & ;;
    *) g++ -std=gnu++17 -O2 -ffp-contract=off -fno-pie -w $INC $DEFS -c "$ROOT/$s" -o "$o" & ;;
    esac
    objs+=("$o")
done
g++ -std=gnu++17 -O2 -fno-pie -w $INC $DEFS -c "$HERE/native.cpp" -o "$OUT/native.o" &
for j in $(jobs -p); do wait "$j" || { echo "selftest: a native compile failed"; exit 1; }; done
g++ -no-pie -o "$OUT/native" "$OUT/native.o" "${objs[@]}" -Wl,--unresolved-symbols=ignore-all -lm

status=0
for pick in ${PICKS:-0 1 2}; do # pick + 256: every value printed
    "$QEMU" "$OUT/c3run" "$D/deqp-c3.c3m" "$N" "$pick" > "$OUT/c3-$pick.txt" 2>&1 || status=1
    "$OUT/native" "$N" "$pick" > "$OUT/native-$pick.txt" 2>&1 || status=1
    tail -1 "$OUT/c3-$pick.txt"
    tail -1 "$OUT/native-$pick.txt"
    grep '^selftest ' "$OUT/c3-$pick.txt" > "$OUT/c3-$pick.lines" || true
    grep '^selftest ' "$OUT/native-$pick.txt" > "$OUT/native-$pick.lines" || true
    differ=$(diff "$OUT/c3-$pick.lines" "$OUT/native-$pick.lines" | grep -c '^[<>]' || true)
    [ -s "$OUT/c3-$pick.lines" ] || differ="all"
    echo "pick $pick: $(wc -l < "$OUT/c3-$pick.lines") cases there, $(wc -l < "$OUT/native-$pick.lines") here; $differ lines differ ($OUT/c3-$pick.txt, native-$pick.txt)"
    [ "$differ" = 0 ] || status=1
done
exit $status
