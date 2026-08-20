#!/bin/sh
# The fixed-point layer must produce identical bits at every optimisation level.
#
# PLAN2.md §7 P2.1's stated test. A unit test cannot check this — it runs at whichever level
# its own binary was built with — so the probe is compiled several times and its output
# compared. `-Ofast` is in the list on purpose: it turns on `-ffast-math`, which is precisely
# the licence to reassociate that makes floating point unreproducible. Integers ignore it.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

CXX=${CXX:-c++}
expected=""
# `-O3 -ffast-math` rather than the deprecated `-Ofast`, which clang now warns about.
for opt in -O0 -O1 -O2 -O3 "-O3 -ffast-math"; do
    # Unquoted on purpose: one entry is two flags.
    # shellcheck disable=SC2086
    "$CXX" -std=c++23 $opt -I"$root/src" \
        "$root/tools/fx_probe/fx_probe.cpp" "$root/src/core/sim/Fx.cpp" \
        -o "$work/probe" || {
        echo "check_fx_optimisation: failed to build at $opt" >&2
        exit 1
    }
    got=$("$work/probe")
    if [ -z "$expected" ]; then
        expected=$got
        echo "  $opt  $got  (reference)"
    elif [ "$got" != "$expected" ]; then
        echo "  $opt  $got  DIFFERS from $expected" >&2
        echo "check_fx_optimisation: the fixed-point layer is not optimisation-independent." >&2
        echo "Something in core/sim/Fx.cpp is relying on undefined or float behaviour." >&2
        exit 1
    else
        echo "  $opt  $got"
    fi
done

echo "check_fx_optimisation: identical at every level."
