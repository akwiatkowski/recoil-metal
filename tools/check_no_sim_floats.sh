#!/bin/sh
# No floating point in the sim's arithmetic (PLAN2.md §5.2, D1).
#
# WHY A GREP AND NOT A REVIEW. The determinism claim in §1.3 — the same command log produces the
# same match on another architecture — is false the moment a tick multiplies two floats. Two
# things break it and neither is fixable by being careful: `libm` is not specified to the last
# bit and implementations disagree there, and the compiler may reassociate and contract. So the
# rule has to be mechanical, and this is the mechanism.
#
# WHAT IS BANNED: a float or double VARIABLE, PARAMETER, RETURN TYPE, or CAST inside
# `src/core/sim`. Those are the shapes that put a float in the middle of a tick.
#
# WHAT IS ALLOWED, and why each is safe:
#
#   1. `constexpr float` CONSTANTS. A constant cannot vary between platforms — it is the same
#      bits in the binary on every machine — so it cannot cause divergence. Content is authored
#      in human units (elmos per second, radians per second, elmos per second squared) and has
#      to be written down somewhere; what matters is that it is converted through a `TickRate`
#      before any arithmetic, which is §5.1's rule and has its own check.
#
#   2. THE BOUNDARY FILES, listed below. Content arrives as floats from the blueprint parser
#      and the renderer draws in floats, so a conversion must exist. Confining it to named
#      files is exactly what makes the rest checkable.
#
# Adding a file to the exemption list is a decision, not a fix. Each one below has a reason.
set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
sim="$root/src/core/sim"

# `Fx`        — the fixed-point types themselves; the conversions in both directions live here.
# `TickRate`  — turns authored per-second floats into per-tick fixed point.
# `Terrain`   — the map states its vertical scale as a float; converted once at construction.
# `Pathfinding` — grid CONSTRUCTION reads float heights and slope limits at load. Its queries
#               (`cellAtWorld`, `worldAtCellCentre`, `findPath`) are fixed point, which is the
#               part that runs in a tick.
# `UnitCatalog` — derives per-tick rates from the per-second floats a blueprint states.
# `Movement`  — `TickClock` bridges wall-clock seconds to ticks, which is its whole job.
# `StateHash` — feeds `Construction::position`, still a float triple until P2.5 moves orders.
# `Economy`   — declares `Construction::position`, same reason.
exempt='Fx\.(hpp|cpp)|TickRate\.(hpp|cpp)|Terrain\.(hpp|cpp)|Pathfinding\.(hpp|cpp)|UnitCatalog\.(hpp|cpp)|Movement\.(hpp|cpp)|StateHash\.cpp|Economy\.hpp'

# Strip comments, then look for float/double in a declaring or casting position.
found=""
for file in "$sim"/*.hpp "$sim"/*.cpp; do
    case "$(basename "$file")" in
        *) if echo "$file" | grep -qE "$exempt"; then continue; fi ;;
    esac

    hits=$(sed -e 's|//.*$||' -e 's|/\*.*\*/||' "$file" \
        | grep -nE '\b(float|double)\b' \
        | grep -vE 'constexpr[[:space:]]+(float|double)' \
        || true)
    if [ -n "$hits" ]; then
        found="$found$(basename "$file"):
$hits
"
    fi
done

if [ -n "$found" ]; then
    echo "check_no_sim_floats: floating point in the sim's arithmetic." >&2
    echo "$found" >&2
    echo "The sim must be fixed point (PLAN2.md §5.2). Use Fx/Mag from core/sim/Fx.hpp;" >&2
    echo "convert content through TickRate at load, not in a tick." >&2
    exit 1
fi

echo "check_no_sim_floats: the sim's arithmetic is fixed point."
