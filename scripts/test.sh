#!/usr/bin/env bash
# Build everything and run the test suite.
#   scripts/test.sh            C++ tests, plus Verilator tests if verilator is found
#   scripts/test.sh --asan     also an ASan/UBSan build of the C++ tests
#   scripts/test.sh --long     the M1 exit run: 100 seeds x 10M messages
set -euo pipefail
cd "$(dirname "$0")/.."

ASAN=0; LONG=0
for a in "$@"; do
  case "$a" in
    --asan) ASAN=1 ;;
    --long) LONG=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

VL=OFF
if command -v verilator >/dev/null 2>&1 || [ -x /opt/verilator/bin/verilator ]; then VL=ON; fi

B=${BUILD:-build}
cmake -S . -B "$B" -G Ninja -DDROMOS_VERILATOR=$VL
cmake --build "$B"
ctest --test-dir "$B" --output-on-failure

if [ $ASAN = 1 ]; then
  cmake -S . -B build-asan -G Ninja -DDROMOS_SANITIZE=ON
  cmake --build build-asan --target test_engine crosscheck
  ./build-asan/test_engine
  ./build-asan/crosscheck --seeds 4 --msgs 200000 --quiet 1
fi

if [ $LONG = 1 ]; then
  ./"$B"/crosscheck --seeds 100 --msgs 10000000 --quiet 1
fi
