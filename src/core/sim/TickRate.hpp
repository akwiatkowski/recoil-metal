#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"

#include <cstdint>

// How often the sim steps, and the only place a duration becomes a number of ticks.
//
// WHY THIS EXISTS (PLAN2.md §5.1, D2). One rule: **every duration and rate is authored once,
// in seconds; the number of ticks is always derived.** No tick-denominated literal exists
// anywhere in the codebase.
//
// The rule kills one specific bug, and the codebase had two examples of it:
//
//     inline constexpr int kProjectileLifetimeTicks = 300;   // "thirty seconds at 10 Hz"
//     static constexpr int kMaxTicksPerAdvance      = 5;     // "a tenth of a second"
//
// Move to 20 Hz and the first silently becomes a fifteen-second projectile lifetime and the
// second a 250 ms catch-up cap. Nothing fails to compile, no test goes red; the balance just
// quietly changes. The rate was recorded in a comment instead of in the arithmetic.
//
// WHY NOT COPY RECOIL. `recoil-engine-map.md §2` opens by calling its unit table "the
// highest-value thing in this document; most conversion bugs are here", and shows why: in one
// unit def, `speed` is elmos per SECOND, `maxVelocity` elmos per FRAME, `maxAcc` elmos per
// frame squared, and `turnRate` 65,536-units per frame. Their unit of time leaked into their
// content format and is now permanent. Ours does not leave the loader.
//
// A `TickRate` is a VALUE, passed to the passes that need it — not a global, not a
// compile-time constant. That is what makes the multi-rate test in P2.3 possible at all: it
// runs the same match at 5, 10, 20 and 50 Hz in one process, which a `constexpr` could not
// express.

namespace rm::sim {

/// A rate-independent duration, as authored.
///
/// A distinct type rather than a bare `float`, so that `rate.ticks(x)` cannot be handed a
/// number of ticks by mistake — the exact confusion the rule above exists to prevent.
///
/// A STRUCT, not the `enum class Seconds : float` that PLAN2 §5.3 specifies: an enumeration's
/// underlying type must be integral, so that declaration is not legal C++ and the plan is
/// wrong on this detail. A one-member struct with an explicit constructor gives the same
/// property — no implicit conversion in either direction — at the same zero cost.
struct Seconds {
    float value = 0.0f;

    constexpr Seconds() noexcept = default;
    explicit constexpr Seconds(float authored) noexcept : value(authored) {}
};

/// Reads as a unit at the call site: `seconds(30.0f)`.
[[nodiscard]] constexpr Seconds seconds(float value) noexcept { return Seconds{value}; }

/// The permitted range, as a hard rule rather than a clamp (D2).
///
/// Out of range is a startup error. A clamp would let a config file ask for 100 Hz, silently
/// get 50, and produce a match nobody can explain; an error names the problem at the moment it
/// is introduced.
inline constexpr std::uint32_t kMinTicksPerSecond = 5;
inline constexpr std::uint32_t kMaxTicksPerSecond = 50;

/// The default: **10 Hz**, because the content this engine imports is Forged Alliance's, whose
/// scripts hardcode it — `WaitSeconds(n)` is `WaitTicks(n * 10 + 1)`
/// (`mohodata/lua/simInit.lua:37`). That `+ 1` was missing from this comment and is not a
/// rounding detail: it inflates every Lua-timed duration in the game by 100 ms, which is what
/// `core/unit/FaDuration.hpp` corrects. Recoil's `GAME_SPEED` is 30 and the two games disagree, so
/// one had to be chosen; nothing here needs 30, since the lockstep multiplayer that fixed both
/// games' rates does not exist in this engine and the renderer was never coupled to the tick.
inline constexpr std::uint32_t kDefaultTicksPerSecond = 10;

/// The sim's clock. Construct once, from configuration; pass it to the passes that need it.
class TickRate {
public:
    /// Throws `std::invalid_argument` outside 5–50 Hz. A constructor rather than a factory
    /// because an invalid `TickRate` must not be representable — a pass that receives one can
    /// then use it without checking.
    explicit TickRate(std::uint32_t ticksPerSecond);

    /// The default rate, for the callers that have no configuration to read.
    TickRate() : TickRate(kDefaultTicksPerSecond) {}

    [[nodiscard]] std::uint32_t ticksPerSecond() const noexcept { return ticksPerSecond_; }

    /// An authored duration, in ticks.
    ///
    /// ROUNDING IS PART OF THE CONTRACT, because it is determinism-relevant: round to nearest,
    /// with a floor of ONE tick for any positive duration. A duration that rounded to zero
    /// would fire every tick, which is not "slightly fast" — it is a different mechanic. A
    /// reload of 0.04 seconds at 10 Hz is 0.4 ticks; one tick is wrong by 150%, and zero ticks
    /// is wrong by infinity.
    [[nodiscard]] TickCount ticks(Seconds duration) const noexcept;

    /// A per-second rate, as the amount that happens in one tick.
    ///
    /// The conversion that must happen exactly once, at content load. A pass that calls this
    /// per tick has put a division in the inner loop and, worse, has kept the per-second value
    /// where a later reader can use it directly.
    [[nodiscard]] Fx perTick(float perSecond) const noexcept;

    /// The same, for a magnitude — income, build rate, upkeep.
    [[nodiscard]] Mag magPerTick(float perSecond) const noexcept;

    /// A per-second angular rate, as binary radians per tick.
    [[nodiscard]] std::int32_t bradPerTick(float radiansPerSecond) const noexcept;

    /// How long a tick lasts. **For the renderer and the HUD clock only** — they are unsynced
    /// display concerns, and a wall-clock second is exactly what they want.
    ///
    /// Deliberately NOT available to sim arithmetic: a pass that multiplies by this has
    /// re-derived a per-tick value that should have been converted at load, and has done it in
    /// floating point. `tools/check_no_sim_floats.sh` is what stops that happening quietly.
    [[nodiscard]] float secondsPerTick() const noexcept;

    [[nodiscard]] friend bool operator==(const TickRate& a, const TickRate& b) noexcept {
        return a.ticksPerSecond_ == b.ticksPerSecond_;
    }

private:
    std::uint32_t ticksPerSecond_ = kDefaultTicksPerSecond;
};

} // namespace rm::sim
