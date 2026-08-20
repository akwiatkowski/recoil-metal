#!/bin/sh
# Generated and maintained by Claude
#
# Asserts that every order into the sim goes through `applyCommand`.
#
# WHY THIS EXISTS (PLAN2.md §7 P4.2). Before P2.5 a human's click called `orderTo` directly and
# the scripted opponent called `orderRouted` directly: two callers, two paths, no record of
# either. Three things were impossible at once — the §1.3 success criterion ("the same command
# log produces the same match") named an artifact that did not exist; the human path could only
# be tested by clicking; and nothing checked that the player issuing an order commanded the
# unit.
#
# P2.5 closed it and P4.1 built the queue on top. This is what keeps it closed. The property is
# structural rather than aspirational only while there is no second way in, and a second way in
# is one convenient call away: `orderAlongPath(motion, path)` is a public function that does
# most of what a move order does, minus the authorisation, minus the record, minus the queue.
#
# So: inside `src/`, only `core/sim/Command.cpp` may call the raw movement orders. Tests may
# call them freely — `test_movement.cpp` exists to test exactly those functions, and forbidding
# that would mean testing them only through three layers that have their own reasons to refuse.

set -eu

root=${1:-.}

# The raw order-setting functions. `orderTo` aims a unit straight at a point; `orderAlongPath`
# sends it along a route. Both write `MoveState` directly.
pattern='orderTo\(|orderAlongPath\('

# Where they are allowed to be called from: their own definitions, and the one function that is
# the path.
allowed='src/core/sim/Movement.cpp|src/core/sim/Movement.hpp|src/core/sim/Command.cpp'

hits=$(grep -rnE "$pattern" "$root/src" 2>/dev/null \
    | grep -vE "$allowed" \
    | grep -vE ':[0-9]+:[[:space:]]*(//|\*|/\*)' \
    || true)

if [ -n "$hits" ]; then
    echo "FAIL: an order bypasses applyCommand."
    echo "$hits" | sed 's/^/        /'
    echo
    echo "Every order goes through sim::applyCommand (PLAN2.md §7 P2.5, P4.2). It is what"
    echo "checks the issuing player, records the command in the log, and puts it in the unit's"
    echo "queue. A call to orderTo or orderAlongPath from anywhere else is a second path, and"
    echo "the whole point is that there is only one — a human's click and a script's decision"
    echo "must not be merely similar."
    exit 1
fi

echo "order paths: one (every order goes through applyCommand)"
