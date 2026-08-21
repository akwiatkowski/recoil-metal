#!/bin/sh
# Generated and maintained by Claude
#
# Asserts that the sim and data layers include nothing from the render side.
#
# WHY THIS EXISTS (PLAN2.md §4, §7 P6.3). §4 names the target shape as four libraries —
# `rm_core` -> `rm_data` -> `rm_sim` -> `rm_render`+app — so that `rm_sim` has no link line to
# Metal and a stray include is a compile error. The libraries are still one CMake target, so
# there is no link line to be missing and no compile error to hit. Until there is, this grep is
# the boundary.
#
# §7 P6.3 states the assertion as "`rm_sim` compiles with `core/scene/` off its include path —
# the real assertion", and it is right that a compiler is the better instrument. This is the
# same claim made with the tool that exists today, and it is registered in CTest so that it is
# checked rather than remembered.
#
# WHAT IT CAUGHT WHEN IT WAS WRITTEN, which is the argument for having it: `core/sim/Army.hpp`
# included `core/scene/TeamColours.hpp` for a `TeamColour` field on `Army`, and
# `core/sim/Movement.hpp` included `core/scene/UnitPlacement.hpp` for a map constant that lives
# in `core/map/HeightField.hpp`. The first was presentation in sim state — two armies with the
# same colour play an identical match, which is the test — and the state hash had a comment
# explaining that it skipped the field. The second was nothing at all: the header it wanted was
# two directories away.
#
# WHAT COUNTS AS THE RENDER SIDE: anything a headless build has no business loading. `scene`
# builds instance and decal buffers, `ui` and `text` build vertices for the HUD, `model`,
# `mesh`, `texture` and `camera` are geometry and pixels. `map` is NOT on the list — a
# heightfield is terrain the sim walks on, and the sim reads it every tick.

set -eu

root=${1:-.}

# Directories that hold presentation. `map` and `lua` and `vfs` are deliberately absent.
forbidden='core/scene/|core/ui/|core/text/|core/model/|core/mesh/|core/texture/|core/camera/|core/render/'

status=0

for dir in sim data unit; do
    target="$root/src/core/$dir"
    if [ ! -d "$target" ]; then
        continue
    fi

    # `#include` lines only — a path named in a COMMENT is documentation, and the comments in
    # `Army.hpp` that explain why the colour is gone would otherwise fail the check that
    # removing it made pass.
    hits=$(grep -rnE "^[[:space:]]*#[[:space:]]*include.*($forbidden)" "$target" || true)
    if [ -n "$hits" ]; then
        echo "FAIL: core/$dir includes the render side."
        echo "$hits" | sed 's/^/        /'
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    echo
    echo "The sim and data layers must not know how anything is drawn (PLAN2.md §4, §7 P6.3)."
    echo "If the thing you need is a fact about the world, it belongs in core/map or core/sim."
    echo "If it is how a fact is DRAWN, the renderer should derive it — an army's colour is the"
    echo "worked example: two armies with the same colour play an identical match."
    exit 1
fi

echo "sim boundary: core/sim, core/data and core/unit are headless"
