#pragma once

// Comparing fixed-point values against the numbers a test author writes.
//
// WHY THIS EXISTS. A test says what it means in decimal — "this unit has 500 hit points" — and
// the sim holds `Mag`/`Fx`. Writing `Mag::fromInt(500)` at every site is noise, and writing
// `magToFloat(h) == Approx(500.0f)` at every site buries the intent under a conversion. Worse,
// the second form quietly picks a tolerance: `Approx`'s default is relative, so it passes on a
// large value and fails on a small one for the same absolute error.
//
// So: one place that converts, with the tolerance stated in units of the type's own last step.
// A fixed-point value is EXACT, so what these check is quantisation — that the value is the
// nearest representable one to what was authored — not that arithmetic is approximately right.

#include "core/sim/Fx.hpp"

#include <catch2/catch_approx.hpp>

namespace rm::test {

/// One step of the fixed-point types. Both use `kFxFractionalBits`, so there is one step.
inline constexpr float kFxStep = 1.0f / (1 << kFxFractionalBits);

/// A `Mag` as the decimal a test author would write, for comparison with `Approx`.
[[nodiscard]] inline float asFloat(sim::Mag value) { return sim::magToFloat(value); }

/// An `Fx` as the decimal a test author would write.
[[nodiscard]] inline float asFloat(sim::Fx value) { return sim::fxToFloat(value); }

/// `CHECK(near(health.current, 500.0f))` — within one representable step of the authored
/// value, which is the most any conversion can promise.
[[nodiscard]] inline Catch::Approx near(sim::Mag value) {
    return Catch::Approx(asFloat(value)).margin(kFxStep);
}
[[nodiscard]] inline Catch::Approx near(sim::Fx value) {
    return Catch::Approx(asFloat(value)).margin(kFxStep);
}

/// The other direction, for building a value from what a test author writes.
[[nodiscard]] inline sim::Mag mag(float authored) { return sim::magFromFloat(authored); }
[[nodiscard]] inline sim::Fx fx(float authored) { return sim::fxFromFloat(authored); }

} // namespace rm::test
