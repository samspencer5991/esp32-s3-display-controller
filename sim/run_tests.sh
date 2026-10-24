#!/bin/sh
# Builds the host targets and runs every automated check:
#   - protocol / controller / app unit tests
#   - touch-driven UI scripts (sim/scripts/*.sim with `expect` lines)
set -e
cd "$(dirname "$0")/.."
cmake -S sim -B build-sim -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-sim -j8 >/dev/null

status=0
echo "== unit tests"
out=$(./build-sim/desk_tests) || status=1
echo "$out" | grep -E "FAIL|checks"
for script in sim/scripts/*.sim; do
    out=$(./build-sim/desk_sim --script "$script" --out build-sim/test-shots 2>&1) || status=1
    echo "$out" | grep -E "FAIL|PASS"
done
[ $status -eq 0 ] && echo "ALL PASSED" || echo "FAILURES"
exit $status
