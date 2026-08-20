#!/bin/sh
# Generated and maintained by Claude
#
# Asserts that the sim holds no file-scope mutable state.
#
# WHY THIS EXISTS. Recoil keeps its sim in global singletons — twelve of them: `gs`, `gsRNG`,
# `unitHandler`, `teamHandler`, `quadField`, `featureHandler`, `projectileHandler`,
# `moveDefHandler`, `losHandler`, `pathManager`, plus `readMap` and `globalRendering`. Their
# reach across `rts/` is not incidental: `gs->` appears in 113 of its 1,463 files,
# `teamHandler` in 78, `unitHandler` in 69.
#
# The consequence that matters to us is not tidiness. It is that you cannot construct two
# sims in one process — and the strongest determinism test there is happens to be exactly
# that: step two of them from one command log, compare hashes each tick, and stop at the
# first divergence, all in one debuggable process. Recoil cannot do it, which is why its own
# sync testing is a file diff between separate runs. We get the better test for free by not
# taking the globals, and this check is what keeps it free.
#
# It also keeps a function's dependencies visible at its signature, which is the property
# that makes a pass testable from two structs and a flat field.
#
# What counts as a violation: a mutable object at file scope. `constexpr` and `const` data
# are fine — they are shared facts, not shared state. A `static` local inside a function is
# also caught, because it is the same hazard wearing a smaller hat: two sims stepping
# through that function would share it.

set -eu

root=${1:-.}

files=$(find "$root/src/core/sim" -type f \( -name '*.cpp' -o -name '*.hpp' \) | sort)

status=0

for file in $files; do
    # File-scope definitions: a line starting in column 1 that declares and is not marked
    # const/constexpr, is not a function or type, and is not a preprocessor line or comment.
    hits=$(grep -nE '^[A-Za-z_][A-Za-z0-9_:<>, *&]*[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(=|;)' "$file" \
        | grep -vE 'constexpr|const |using |struct |class |enum |namespace |return |typedef' \
        | grep -vE '^\s*[0-9]+:\s*//' || true)
    if [ -n "$hits" ]; then
        echo "FAIL: $file has file-scope mutable state."
        echo "$hits" | sed 's/^/        /'
        status=1
    fi

    # `static` locals: same hazard, function scope.
    #
    # Comment lines are skipped, the same way the file-scope check above skips them. That is
    # not a loophole: a comment is not state, and the alternative is that describing what
    # Recoil's `static` command-description cache does fails the check that exists because of
    # it. A `static` sharing a line with real code is still caught by the check above.
    statics=$(grep -nE '(^|[[:space:]])static[[:space:]]' "$file" \
        | grep -vE 'constexpr|const |static_cast|static_assert' \
        | grep -vE '^\s*[0-9]+:\s*(//|\*|/\*)' || true)
    if [ -n "$statics" ]; then
        echo "FAIL: $file has mutable static state."
        echo "$statics" | sed 's/^/        /'
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    echo
    echo "The sim is a value you construct, not a process-wide singleton set (PLAN2.md"
    echo "§5.4). Pass what a pass needs; make the RNG a member seeded from the match setup."
    echo "Two sims must be able to exist at once — the divergence harness depends on it."
    exit 1
fi

echo "sim globals: none (two sims can coexist in one process)"
