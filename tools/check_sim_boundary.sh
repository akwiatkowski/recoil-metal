#!/bin/sh
# Generated and maintained by Claude
#
# Asserts that no caller hand-rolls the sim's tick order.
#
# WHY THIS EXISTS. Every rule a match is made of was tested, and the ORDER they run in was
# not — it lived in an anonymous namespace in `main.mm`, which no test can link. So the two
# loops diverged: the windowed frame loop ticked movement and collisions only, a unit in
# the interactive game moved perfectly and never fired a shot, and the whole suite stayed
# green the entire time.
#
# `rm::sim::tickSkirmish` owns that order now, and `MatchRunner` owns the caller's side of
# it. This check is what keeps them the only way in: the passes below MUTATE match state,
# and a second caller reaching past them is exactly how the divergence happened. Catching
# it costs a grep; catching it by playing the game costs a milestone.
#
# Pure reads are deliberately absent from the list — `winningTeam`, `deadUnits` and
# `countCommanders` answer questions without advancing anything, and a caller asking them
# is reporting, not ticking.
#
# `tests/` is exempt by construction: it is not searched. A unit test for movement SHOULD
# call the movement pass directly — that is what makes it a unit test.

set -eu

root=${1:-.}

# The passes that advance match state. Matched with a trailing '(' so a mention in a
# comment or a declaration does not trip the check.
passes='
resolveCollisions(
aimAtTargets(
fireWeapons(
advanceProjectiles(
tickEconomy(
applyDefeats(
explodeOnDeath(
takeFinished(
damageArea(
'

# Where a caller lives: everything under src/ that is not the sim itself.
callers=$(find "$root/src" -type f \( -name '*.mm' -o -name '*.cpp' -o -name '*.hpp' \) \
    | grep -v '/core/sim/' | sort)

status=0

for pass in $passes; do
    for file in $callers; do
        if grep -Fn -- "$pass" "$file" >/dev/null 2>&1; then
            echo "FAIL: $file calls $pass directly."
            grep -Fn -- "$pass" "$file" | sed 's/^/        /'
            status=1
        fi
    done
done

# `sim::tick(` needs its namespace to avoid matching every `.tick(` in the tree.
for file in $callers; do
    if grep -Fn -- 'sim::tick(' "$file" >/dev/null 2>&1; then
        echo "FAIL: $file calls sim::tick( directly."
        grep -Fn -- 'sim::tick(' "$file" | sed 's/^/        /'
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    echo
    echo "A match is advanced through rm::sim::tickSkirmish (the sim's pass order) and"
    echo "MatchRunner/advanceMatch in main.mm (the caller's). Add the work there, so that"
    echo "both the headless pre-run and the windowed frame loop get it."
    exit 1
fi

echo "sim boundary: no caller hand-rolls the tick order"
