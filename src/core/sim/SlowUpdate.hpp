#pragma once

#include "core/Types.hpp"
#include "core/sim/TickRate.hpp"

#include <cstddef>

namespace rm::sim {

// Periodic work, spread across the ticks of its own period instead of landing on one of them.
//
// WHY THIS EXISTS (PLAN2.md §6.7, §7 P3.6). Some work does not need doing every tick — picking
// a target, re-checking a build site, deciding what to build next. Recoil calls that a
// `SlowUpdate` and runs it on a **hardcoded 15-frame cycle** — `UNIT_SLOWUPDATE_RATE = 15`
// (`Sim/Misc/GlobalConstants.h:60`), applied as `gs->frameNum % UNIT_SLOWUPDATE_RATE`
// (`UnitHandler.cpp:355`) — staggering objects across the frames of the cycle so that no
// single frame does all of it.
//
// Two things are worth copying and one is not:
//
//   COPY the period: doing slow work slowly is right, and the alternative — every tick — is
//   what makes an O(n²) sweep an O(n²) sweep *per tick*.
//
//   COPY the stagger: a period without one just moves the whole cost onto every fifteenth
//   frame, which is a stutter rather than a saving. `--bench` sees this directly, and it is
//   §7 P3.6's stated manual check.
//
//   DO NOT copy the 15. It is a number of FRAMES, so Recoil's own period is 0.5 s at
//   `GAME_SPEED 30` and would silently become 3.0 s at our minimum rate and 0.3 s at our
//   maximum. That is precisely the bug §5.1 exists to kill, and `check_no_tick_literals.sh`
//   exists to catch. The period here is authored in SECONDS by the caller and the tick count
//   is derived — and there is deliberately no default, because a period is a claim about the
//   work being paced and not a property of pacing.
//
// WHY IT MATTERS NOW, in the plan's own words: `BuildOrder`'s callers already hand-rolled
// "every second rather than every tick" as `tickIndex % decisionTicks() == 0`. That is this
// pattern, discovered locally and without the stagger. Making it a mechanism is what stops the
// next four features each inventing it again — and each getting the derivation wrong in its
// own way.
//
// THE GUARANTEE, which is what the test asserts: over any window of `period()` consecutive
// ticks, every index is due **exactly once**. Not at most once, and not on average once. That
// holds however many indices there are, and it keeps holding as they come and go — which
// matters because the flat unit store never moves a live unit's slot (`UnitStore`), so an
// index's place in the rotation is fixed for as long as it exists. A store that compacted on
// death would reshuffle the rotation and could skip or double-visit whoever moved.

/// Which indices are due on which tick.
///
/// A VALUE, cheap to copy and holding nothing but its period — so a pass takes one the same
/// way it takes a `TickRate`, and two subsystems pacing at different periods are two of these
/// rather than one with a mode.
class SlowUpdate {
public:
    /// The period, authored in seconds and converted once.
    ///
    /// `TickRate::ticks` floors at one tick, so an absurdly short period degrades to "every
    /// tick, every index" rather than to a division by zero. That is the honest failure: work
    /// paced faster than the clock cannot be paced.
    SlowUpdate(TickRate rate, Seconds period) noexcept
        : period_(rate.ticks(period)) {}

    /// How many ticks one full rotation takes.
    [[nodiscard]] TickCount period() const noexcept { return period_; }

    /// Whether this index's turn is this tick.
    ///
    /// `index % period == tick % period`: the residue is a property of the INDEX, so it does
    /// not drift, and no state is stored anywhere. That is what makes this safe to ask from
    /// several passes in one tick and safe to reconstruct mid-match — a counter would have to
    /// be hashed, replayed and kept in step with spawns.
    [[nodiscard]] bool due(std::size_t index, TickIndex tick) const noexcept {
        return index % period_ == tick % period_;
    }

    /// How many of `total` indices are due on this tick. For a bench or a test that wants the
    /// distribution rather than the membership — the claim "no tick does all the work" is a
    /// statement about this number.
    [[nodiscard]] std::size_t dueCount(std::size_t total, TickIndex tick) const noexcept;

private:
    /// Never zero: `TickRate::ticks` floors at one.
    TickCount period_ = 1;
};

} // namespace rm::sim
