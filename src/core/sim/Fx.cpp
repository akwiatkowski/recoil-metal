#include "core/sim/Fx.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>

namespace rm::sim {
namespace {

// CORDIC works at higher precision than it reports, so that thirty iterations of accumulated
// rounding stay below the last bit of a Q18.14 answer. Everything below is Q*.30 in `FxWide`.
inline constexpr int kInternalBits = 30;

/// The angle domain used inside the iteration: a full turn is 2^32.
///
/// Wider than `Brad`'s 2^16 for the same reason `kInternalBits` is wider than
/// `kFxFractionalBits` — the angle is driven to zero by subtracting ever-smaller constants,
/// and those constants have to stay non-zero for the iteration to keep converging. At turn/2^32
/// the thirtieth is 1; at turn/2^16 it would have reached zero by the fourteenth and the
/// remaining sixteen iterations would do nothing.
inline constexpr int kAngleShift = 16;  // Brad → internal
inline constexpr FxWide kQuarterTurnInternal = FxWide{1} << 30;
inline constexpr FxWide kHalfTurnInternal = FxWide{1} << 31;

/// `atan(2^-i)` as a fraction of a full turn, scaled by 2^32.
///
/// Generated, not hand-written:
///     angles[i] = round(atan(2**-i) / (2*pi) * 2**32)
/// Embedded rather than computed at startup because a table computed by `libm` would put
/// exactly the dependency this file exists to remove into the sim's constants — two machines
/// whose `atan` differed in the last bit would build different tables and then diverge for a
/// reason no replay could explain.
inline constexpr std::array<FxWide, 30> kAtanTable{
    536870912, 316933406, 167458907, 85004756, 42667331, 21354465, 10679838, 5340245,
    2670163,   1335087,   667544,    333772,   166886,   83443,    41722,    20861,
    10430,     5215,      2608,      1304,     652,      326,      163,      81,
    41,        20,        10,        5,        3,        1,
};

/// The CORDIC gain, `prod(1/sqrt(1 + 2^-2i))` over the thirty iterations, as Q*.30.
///
/// Each iteration is a rotation composed with a scaling by `sqrt(1 + 2^-2i)`; the rotations do
/// the work and the scalings are a constant that can be divided out once. Pre-loading `x` with
/// the gain means the result comes out already normalised. Converges to 0.6072529350088813 —
/// the classic CORDIC constant.
inline constexpr FxWide kGain = 652032874;

// NOTE THE DIRECTION OF THE GAIN, because getting it backwards is silent and produces
// something that still looks like a length. Each iteration is a rotation composed with a
// scaling by `sqrt(1 + 2^-2i)`, which is GREATER than one — so over the run the magnitude
// GROWS, by 1/K. Rotation mode therefore pre-loads `x` with K so the growth lands on one;
// vectoring mode must MULTIPLY the surviving `x` by K to undo it. Multiplying by 1/K instead
// gives an answer 2.71x too large — (1/K)^2 — which is what the 3-4-5 triangle case caught.

/// Rotation mode: drives the angle to zero, so `(x, y)` ends up at `(cos, sin)`.
///
/// Returned as a pair in internal precision; the callers round once, at the end, which is why
/// there is only one rounding step in the whole computation.
struct SinCos {
    FxWide cos;
    FxWide sin;
};

[[nodiscard]] SinCos cordicRotate(Brad angle) noexcept {
    // Brad → internal, as a SIGNED angle. `Brad` is unsigned and covers a full turn, so the
    // top half of its range is the negative half of the circle.
    FxWide z = FxWide{angle} << kAngleShift;
    if (z >= kHalfTurnInternal) {
        z -= kHalfTurnInternal * 2;
    }

    // CORDIC only converges within about ±99.9° of zero, so the two far quadrants are folded
    // in by a half-turn rotation — which negates both components exactly, costing nothing.
    bool negate = false;
    if (z > kQuarterTurnInternal) {
        z -= kHalfTurnInternal;
        negate = true;
    } else if (z < -kQuarterTurnInternal) {
        z += kHalfTurnInternal;
        negate = true;
    }

    FxWide x = kGain;
    FxWide y = 0;
    for (int i = 0; i < static_cast<int>(kAtanTable.size()); ++i) {
        // A rotation by `atan(2^-i)`, which in these coordinates is two shifts and two adds —
        // the trick the whole algorithm rests on.
        const FxWide dx = x >> i;
        const FxWide dy = y >> i;
        if (z >= 0) {
            x -= dy;
            y += dx;
            z -= kAtanTable[static_cast<std::size_t>(i)];
        } else {
            x += dy;
            y -= dx;
            z += kAtanTable[static_cast<std::size_t>(i)];
        }
    }

    return negate ? SinCos{-x, -y} : SinCos{x, y};
}

/// Vectoring mode: drives `y` to zero, so the accumulated angle is the vector's bearing and
/// the surviving `x` is its length times the gain.
struct Vector2 {
    FxWide angle;  // internal angle units
    FxRaw length;  // Q18.14, gain already removed
};

/// `axisX` is the component along the reference axis, `axisY` the one across it. Both are
/// `Fx` raw values; the length comes back as an `Fx` raw value too.
[[nodiscard]] Vector2 cordicVector(FxRaw axisX, FxRaw axisY) noexcept {
    // NORMALISE BY A POWER OF TWO FIRST, which buys two things at once.
    //
    // Precision: CORDIC's per-iteration shift is `>> i`, so a small input runs out of
    // significant bits long before the thirtieth iteration and the answer for a
    // one-raw-unit vector would be noise. Shifting the larger component up to the top of the
    // safe range means every magnitude gets the same ~32 bits of working precision.
    //
    // Headroom: the gain correction at the end is a 64-bit multiply, and it is the tightest
    // constraint in this file. `x` leaves the loop at up to `1.42 / K` times the normalised
    // input, so with the input capped at 2^32 the product with `kGain` peaks at 6.5e18 —
    // measured, against `int64`'s 9.2e18. Promoting the input to 30 fractional bits like
    // rotation mode does would overflow it by a factor of a thousand.
    const FxWide magnitude =
        std::max(std::abs(FxWide{axisX}), std::abs(FxWide{axisY}));
    const int shift = 32 - static_cast<int>(std::bit_width(static_cast<std::uint64_t>(
                               magnitude)));

    FxWide x = FxWide{axisX} << shift;
    FxWide y = FxWide{axisY} << shift;

    // Same convergence limit as rotation mode: fold the left half-plane over by a half turn.
    FxWide z = 0;
    if (x < 0) {
        x = -x;
        y = -y;
        z = kHalfTurnInternal;
    }

    for (int i = 0; i < static_cast<int>(kAtanTable.size()); ++i) {
        const FxWide dx = x >> i;
        const FxWide dy = y >> i;
        if (y <= 0) {
            x -= dy;
            y += dx;
            z -= kAtanTable[static_cast<std::size_t>(i)];
        } else {
            x += dy;
            y -= dx;
            z += kAtanTable[static_cast<std::size_t>(i)];
        }
    }

    // `x` is now `hypot / K`, scaled up by the normalising shift. Both come out in one
    // rounding: multiply by K, then shift back down by the gain's own fractional bits plus
    // the shift that was applied to the input.
    return Vector2{z, saturate(roundShift(x * kGain, kInternalBits + shift))};
}

/// Integer square root of a 64-bit value, bit by bit, most significant first.
///
/// Exact: the result is the largest `r` with `r*r <= value`. Terminates in 32 steps regardless
/// of input, which is what makes it safe in a sim — a loop whose length depends on the data is
/// a loop that can be made to run long.
[[nodiscard]] std::uint64_t integerSqrt(std::uint64_t value) noexcept {
    std::uint64_t result = 0;
    // The highest power of four that fits, so the first candidate bit is the top one.
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

} // namespace

Fx fxSin(Brad angle) noexcept {
    return Fx::fromRaw(saturate(roundShift(cordicRotate(angle).sin, kInternalBits
                                                                       - kFxFractionalBits)));
}

Fx fxCos(Brad angle) noexcept {
    return Fx::fromRaw(saturate(roundShift(cordicRotate(angle).cos, kInternalBits
                                                                       - kFxFractionalBits)));
}

Polar fxPolar(Fx x, Fx z) noexcept {
    if (x.raw() == 0 && z.raw() == 0) {
        // A zero vector has no bearing. Zero is the answer that makes a unit sitting exactly
        // on its target keep facing where it already faces, rather than snapping to +Z.
        return Polar{.bearing = 0, .length = kFxZero};
    }

    // NOTE THE AXES. This engine measures bearing from +Z toward +X, so +Z is the CORDIC
    // reference axis and +X is the one across it — the swap that `Combat.cpp` warns about,
    // done once here instead of at every call site.
    const Vector2 result = cordicVector(z.raw(), x.raw());

    // Internal angle → Brad. Rounded rather than shifted, so the answer stays symmetric about
    // zero; then masked, which is the wraparound `Brad` exists for.
    const FxWide bradSigned = roundShift(result.angle, kAngleShift);
    return Polar{
        .bearing = static_cast<Brad>(static_cast<std::uint32_t>(bradSigned) & 0xFFFFu),
        .length = Fx::fromRaw(result.length),
    };
}

Brad fxBearing(Fx x, Fx z) noexcept { return fxPolar(x, z).bearing; }

Fx fxHypot(Fx x, Fx z) noexcept { return fxPolar(x, z).length; }

Brad fxAsin(Fx value) noexcept {
    // asin(v) = atan2(v, sqrt(1 - v^2)), which is one vectoring pass over the right triangle
    // with opposite `v` and adjacent `sqrt(1 - v^2)`.
    if (value >= kFxOne) {
        return kBradQuarterTurn;
    }
    if (value <= -kFxOne) {
        return static_cast<Brad>(-static_cast<int>(kBradQuarterTurn));
    }

    const Fx adjacent = fxSqrt(kFxOne - value * value);
    // Adjacent along the reference axis, opposite across it — the same axis order as
    // `fxPolar`, whose first argument is the across-axis component.
    return fxPolar(value, adjacent).bearing;
}

Fx fxSqrt(Fx value) noexcept {
    if (value.raw() <= 0) {
        return kFxZero;
    }
    // sqrt of a Q18.14 value is sqrt(raw / 2^14) = sqrt(raw * 2^14) / 2^14, so the operand is
    // shifted UP by the fractional bits before the integer root. `raw` is at most 2^31, so the
    // shifted operand is at most 2^45 — well inside the 62 bits `integerSqrt` starts from.
    const auto operand = static_cast<std::uint64_t>(FxWide{value.raw()} << kFxFractionalBits);
    return Fx::fromRaw(saturate(static_cast<FxWide>(integerSqrt(operand))));
}

// --- The float boundary ---------------------------------------------------------------

Fx fxFromFloat(float value) noexcept {
    // `std::lround` rather than a cast, because a cast truncates toward zero and would make
    // the conversion asymmetric about the origin. This is authoring-time code — it runs at
    // catalog load, not in a tick — so the `libm` call is outside the determinism claim.
    return Fx::fromRaw(saturate(static_cast<FxWide>(
        std::llround(static_cast<double>(value) * (1 << kFxFractionalBits)))));
}

Mag magFromFloat(float value) noexcept {
    return Mag::fromRaw(
        std::llround(static_cast<double>(value) * (1 << kFxFractionalBits)));
}

float fxToFloat(Fx value) noexcept {
    return static_cast<float>(value.raw()) / static_cast<float>(1 << kFxFractionalBits);
}

float magToFloat(Mag value) noexcept {
    return static_cast<float>(static_cast<double>(value.raw())
                              / static_cast<double>(1 << kFxFractionalBits));
}

Brad bradFromRadians(float radians) noexcept {
    // A full turn is 2^16 brad, so the conversion is `radians / (2*pi) * 65536`. The result is
    // reduced modulo a full turn by the cast to `Brad`, which is exactly the wraparound the
    // type was chosen for.
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const double turns = static_cast<double>(radians) / kTwoPi;
    const long long scaled = std::llround(turns * 65536.0);
    return static_cast<Brad>(static_cast<std::uint32_t>(scaled) & 0xFFFFu);
}

float radiansFromBrad(Brad angle) noexcept {
    constexpr float kTwoPi = 6.28318530717958647692f;
    return static_cast<float>(angle) * (kTwoPi / 65536.0f);
}

} // namespace rm::sim
