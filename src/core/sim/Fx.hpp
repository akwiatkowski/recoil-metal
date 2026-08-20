#pragma once

#include "core/Types.hpp"

#include <compare>
#include <cstdint>

// Fixed-point arithmetic, and the only trigonometry the sim is allowed.
//
// WHY THIS EXISTS. The project's success criterion is that the same command log produces the
// same match on this machine and on another architecture (PLAN2.md §1.3). Floating point does
// not give that. Two things break it, and neither is fixable by being careful:
//
//   1. **`libm` is not standardised to the last bit.** `sin`, `cos`, `atan2`, `pow` and
//      friends are permitted to differ between implementations, and they do — Apple's
//      libm, glibc and musl disagree in the final bits for the same input. A sim that calls
//      `std::cos` has outsourced its determinism to whichever C library the machine shipped.
//   2. **The compiler is allowed to re-associate and to contract.** `a*b + c` may become a
//      fused multiply-add with one rounding instead of two, at the optimiser's discretion, so
//      the same source can produce different results at `-O0` and `-O2` on one machine.
//
// Integers have neither problem. `+`, `-`, `*`, `/` and `>>` on `int32`/`int64` are exactly
// specified by the standard, and C++20 onward guarantees two's complement and arithmetic
// right-shift for signed types — so every operation below produces the same bits everywhere,
// by construction rather than by testing.
//
// LAYOUT — and it diverges from PLAN2 §5.2's Q16.16, on measurement. See `core/Types.hpp` for
// the two corpus facts that force it: three shipped maps are 32,768 elmos across (exactly
// Q16.16's ceiling) and `BuildCostEnergy` reaches 10,008,000. So:
//
//   - `Fx`  — Q18.14 in `int32`. Geometry: position, velocity, radius, speed.
//   - `Mag` — Q50.14 in `int64`. Magnitudes: health, resources, costs, damage.
//   - `Brad` — an angle as turn/65,536 (`core/Types.hpp`), which wraps by unsigned overflow.
//
// `Fx` and `Mag` share `kFxFractionalBits`, so converting between them is a widening or a
// narrowing and never a rescale.
//
// THIS IS THE ONE FILE IN THE SIM THAT MAY NAME `float`. The conversions at the bottom are
// the boundary: content is authored in floats and the renderer draws in floats, so the
// conversion has to live somewhere, and having it live in exactly one file is what makes
// "no float in the sim" a thing a script can check. Every other file in `core/sim/` is
// float-free once P2.2 finishes, and `tools/check_no_sim_floats.sh` enforces it.

namespace rm::sim {

/// Rounds a widened intermediate back down by `bits`, to nearest.
///
/// Round-to-nearest rather than truncation, and the tie goes toward +infinity. Both halves of
/// that are arbitrary; what matters is that they are FIXED. A rounding rule that varied with
/// the sign, the platform or the optimiser would be the same class of bug as calling `libm`.
///
/// `>>` on a negative signed value is an arithmetic shift — guaranteed since C++20, which is
/// why this is written as a shift rather than a division. Division truncates toward zero,
/// which would make the rounding asymmetric about the origin for no gain.
[[nodiscard]] constexpr FxWide roundShift(FxWide value, int bits) noexcept {
    return (value + (FxWide{1} << (bits - 1))) >> bits;
}

/// Narrows to `FxRaw`, SATURATING rather than wrapping.
///
/// Wrapping would turn an overflowing positive into a large negative — a unit that teleports
/// to the opposite corner of the map, which is a bug that looks like a physics bug. Saturation
/// is also wrong when it happens, but it is wrong in the direction the value was already
/// going, and it is deterministic. Neither is as good as not overflowing: `Fx`'s range is
/// four times the biggest shipped map precisely so this stays unreachable in practice.
[[nodiscard]] constexpr FxRaw saturate(FxWide value) noexcept {
    constexpr FxWide lo = FxWide{INT32_MIN};
    constexpr FxWide hi = FxWide{INT32_MAX};
    return static_cast<FxRaw>(value < lo ? lo : (value > hi ? hi : value));
}

/// A geometric quantity: Q18.14 fixed point. Range ±131,072, resolution 1/16,384.
class Fx {
public:
    constexpr Fx() noexcept = default;

    /// From the underlying integer. Named rather than a constructor so that a raw value can
    /// never be mistaken for a whole number at a call site — `Fx{3}` reading as three
    /// sixteen-thousandths would be a very quiet bug.
    [[nodiscard]] static constexpr Fx fromRaw(FxRaw raw) noexcept {
        Fx result;
        result.raw_ = raw;
        return result;
    }

    /// From a whole number.
    [[nodiscard]] static constexpr Fx fromInt(std::int32_t whole) noexcept {
        return fromRaw(saturate(FxWide{whole} << kFxFractionalBits));
    }

    /// From a ratio of whole numbers — `fromRatio(1, 3)` is a third.
    ///
    /// How a constant is authored without writing a float. `Fx::fromRatio(1, 2)` says a half
    /// exactly; `Fx::fromFloat(0.5f)` says the same thing but drags the float boundary into
    /// a header that has no other reason to touch one.
    [[nodiscard]] static constexpr Fx fromRatio(std::int32_t numerator,
                                                std::int32_t denominator) noexcept {
        // Rounded to nearest like every other operation here, so `fromRatio(2, 3)` is the
        // closest representable two-thirds rather than the one below it. A ratio that is not
        // representable stays not representable: `fromRatio(1, 3) * 3` is one step short of
        // one, and that is quantisation rather than a bug to round away.
        return fromRaw(divide(saturate(FxWide{numerator} << kFxFractionalBits),
                              saturate(FxWide{denominator} << kFxFractionalBits)));
    }

    [[nodiscard]] constexpr FxRaw raw() const noexcept { return raw_; }

    /// Truncated toward negative infinity, so that `floor(-0.5) == -1`. This is the sim's
    /// `std::floor`, and it is a shift rather than a call.
    [[nodiscard]] constexpr std::int32_t floorToInt() const noexcept {
        return static_cast<std::int32_t>(FxWide{raw_} >> kFxFractionalBits);
    }

    constexpr Fx& operator+=(Fx other) noexcept {
        raw_ = saturate(FxWide{raw_} + other.raw_);
        return *this;
    }
    constexpr Fx& operator-=(Fx other) noexcept {
        raw_ = saturate(FxWide{raw_} - other.raw_);
        return *this;
    }
    constexpr Fx& operator*=(Fx other) noexcept {
        raw_ = saturate(roundShift(FxWide{raw_} * other.raw_, kFxFractionalBits));
        return *this;
    }
    constexpr Fx& operator/=(Fx other) noexcept {
        raw_ = divide(raw_, other.raw_);
        return *this;
    }

    [[nodiscard]] friend constexpr Fx operator+(Fx a, Fx b) noexcept { return a += b; }
    [[nodiscard]] friend constexpr Fx operator-(Fx a, Fx b) noexcept { return a -= b; }
    [[nodiscard]] friend constexpr Fx operator*(Fx a, Fx b) noexcept { return a *= b; }
    [[nodiscard]] friend constexpr Fx operator/(Fx a, Fx b) noexcept { return a /= b; }
    [[nodiscard]] constexpr Fx operator-() const noexcept { return fromRaw(-raw_); }

    /// Scaling by a plain integer, which needs no rounding and cannot lose a fraction.
    [[nodiscard]] friend constexpr Fx operator*(Fx a, std::int32_t n) noexcept {
        return fromRaw(saturate(FxWide{a.raw_} * n));
    }
    [[nodiscard]] friend constexpr Fx operator*(std::int32_t n, Fx a) noexcept { return a * n; }

    [[nodiscard]] friend constexpr bool operator==(Fx a, Fx b) noexcept {
        return a.raw_ == b.raw_;
    }
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(Fx a, Fx b) noexcept {
        return a.raw_ <=> b.raw_;
    }

private:
    /// Round-to-nearest signed division, ties away from zero.
    ///
    /// Dividing by zero returns the saturated extreme rather than trapping: a sim that
    /// divided by zero has a bug either way, and a deterministic wrong answer can be found by
    /// the replay harness while a signal cannot.
    [[nodiscard]] static constexpr FxRaw divide(FxRaw a, FxRaw b) noexcept {
        if (b == 0) {
            return a >= 0 ? INT32_MAX : INT32_MIN;
        }
        // Doubled, so the halfway case is visible in the low bit before it is discarded.
        // `a` is at most ~5.4e8 raw, so `a << 15` is ~1.8e13 — nowhere near 64 bits.
        const FxWide doubled = ((FxWide{a} << (kFxFractionalBits + 1)) / b);
        return saturate(doubled >= 0 ? (doubled + 1) / 2 : (doubled - 1) / 2);
    }

    FxRaw raw_ = 0;
};

/// A magnitude: Q50.14 fixed point. Range ±5.6e14, resolution 1/16,384.
///
/// Deliberately a thinner type than `Fx`. Health and resources are added, subtracted, compared
/// and clamped; they are not rotated, projected or normalised, so the operations that need
/// care about rounding do not arise. Multiplication by an `Fx` is how a rate becomes an
/// amount, and that is the only mixed operation.
class Mag {
public:
    constexpr Mag() noexcept = default;

    [[nodiscard]] static constexpr Mag fromRaw(MagRaw raw) noexcept {
        Mag result;
        result.raw_ = raw;
        return result;
    }
    [[nodiscard]] static constexpr Mag fromInt(std::int64_t whole) noexcept {
        return fromRaw(whole << kFxFractionalBits);
    }
    [[nodiscard]] static constexpr Mag fromFx(Fx value) noexcept {
        // A widening, not a rescale — the two share their fractional bits, which is the whole
        // reason they were given the same count.
        return fromRaw(value.raw());
    }

    [[nodiscard]] constexpr MagRaw raw() const noexcept { return raw_; }

    /// Narrows to `Fx`, saturating. For handing a magnitude to geometry — a damage figure
    /// used as a radius, say — where the value is known to be small.
    [[nodiscard]] constexpr Fx toFx() const noexcept { return Fx::fromRaw(saturate(raw_)); }

    [[nodiscard]] constexpr std::int64_t floorToInt() const noexcept {
        return raw_ >> kFxFractionalBits;
    }

    constexpr Mag& operator+=(Mag other) noexcept {
        raw_ += other.raw_;
        return *this;
    }
    constexpr Mag& operator-=(Mag other) noexcept {
        raw_ -= other.raw_;
        return *this;
    }
    /// Scaling by a fraction: `health * Fx::fromRatio(1, 2)` is half of it.
    constexpr Mag& operator*=(Fx scale) noexcept {
        raw_ = roundShift(raw_ * scale.raw(), kFxFractionalBits);
        return *this;
    }

    [[nodiscard]] friend constexpr Mag operator+(Mag a, Mag b) noexcept { return a += b; }
    [[nodiscard]] friend constexpr Mag operator-(Mag a, Mag b) noexcept { return a -= b; }
    [[nodiscard]] friend constexpr Mag operator*(Mag a, Fx b) noexcept { return a *= b; }
    [[nodiscard]] constexpr Mag operator-() const noexcept { return fromRaw(-raw_); }

    [[nodiscard]] friend constexpr bool operator==(Mag a, Mag b) noexcept {
        return a.raw_ == b.raw_;
    }
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(Mag a, Mag b) noexcept {
        return a.raw_ <=> b.raw_;
    }

private:
    MagRaw raw_ = 0;
};

// --- Constants ------------------------------------------------------------------------

inline constexpr Fx kFxZero = Fx::fromInt(0);
inline constexpr Fx kFxOne = Fx::fromInt(1);

/// A quarter turn, a half turn, and a full turn, in binary radians.
///
/// A full turn is 65,536, which does not fit a `Brad` — that is the point. Adding
/// `kBradFullTurn` to an angle is the identity by unsigned wraparound, so the type cannot
/// hold an out-of-range angle and no wrap step is ever needed.
inline constexpr Brad kBradQuarterTurn = 16384;
inline constexpr Brad kBradHalfTurn = 32768;
inline constexpr std::uint32_t kBradFullTurn = 65536;

// --- Trigonometry ---------------------------------------------------------------------
//
// All four come from ONE algorithm: CORDIC, which computes a rotation with nothing but
// additions, subtractions and shifts of integers, plus a table of thirty constants. In
// rotation mode it turns an angle into a sine and cosine; in vectoring mode it turns a vector
// into an angle and a length, which is `atan2` and `hypot` at once.
//
// Sources:
//   - Volder, J. E. (1959), "The CORDIC Trigonometric Computing Technique", *IRE Transactions
//     on Electronic Computers* EC-8(3), 330–334 — the original.
//   - Andraka, R. (1998), "A survey of CORDIC algorithms for FPGA based computers",
//     *Proceedings of the 1998 ACM/SIGDA sixth international symposium on FPGAs*, 191–200 —
//     the practical treatment, including the scaling constant and convergence range.
//
// Chosen over a lookup table because a table for 65,536 angles at useful precision is a large
// constant array whose accuracy is fixed at authoring time, while CORDIC is thirty lines and
// converges to below the output's last bit. Measured against `libm` over the whole circle, the
// worst error is 1.2e-8 — about 1/5000th of a `Q18.14` step.

/// Sine of an angle. Exact-ranged: every `Brad` is a valid input, because the type cannot
/// hold an invalid one.
[[nodiscard]] Fx fxSin(Brad angle) noexcept;

/// Cosine of an angle.
[[nodiscard]] Fx fxCos(Brad angle) noexcept;

/// The angle of the vector `(x, z)`, measured from +Z toward +X.
///
/// NOT the mathematical `atan2(y, x)` convention. This engine measures a unit's yaw from +Z
/// toward +X because that is what the vertex shader does with it, so the arguments are in
/// that order and the name says so — `Combat.cpp`'s `bearingTo` had a comment warning that
/// swapping them "compiles, runs, and points every turret ninety degrees off". A signature
/// that names its axes cannot be called the wrong way round.
[[nodiscard]] Brad fxBearing(Fx x, Fx z) noexcept;

/// The length of the vector `(x, z)`. Falls out of the same computation as `fxBearing`, so
/// callers that want both should use `fxPolar`.
[[nodiscard]] Fx fxHypot(Fx x, Fx z) noexcept;

/// A vector's angle and length together, which is one CORDIC pass rather than two.
struct Polar {
    Brad bearing;
    Fx length;
};
[[nodiscard]] Polar fxPolar(Fx x, Fx z) noexcept;

/// Arcsine, for a value clamped to [-1, 1]. Out-of-range inputs saturate to ±a quarter turn
/// rather than being undefined, because the caller that produces one is normalising a vector
/// and rounding took it a hair past one.
[[nodiscard]] Brad fxAsin(Fx value) noexcept;

/// Square root. Zero for a negative input — a negative length has no meaning, and returning
/// something deterministic beats a NaN that propagates silently into the state hash.
///
/// Bit-by-bit integer square root rather than Newton's method: it needs no initial guess, it
/// terminates in a fixed number of steps, and it is exact to the last bit. Newton would be
/// faster and would make the answer depend on how many iterations were run.
[[nodiscard]] Fx fxSqrt(Fx value) noexcept;

// --- The float boundary ---------------------------------------------------------------
//
// These four are why this file is exempt from the no-float rule. Content arrives as floats
// out of the blueprint parser and the renderer wants floats to upload, so the conversion has
// to exist; keeping it here means every other file in `core/sim/` can be checked mechanically.
//
// A conversion is a ONE-WAY DOOR in each direction. Content converts to fixed point once, at
// catalog load, and the sim never sees the float again (PLAN2 §5.1). Sim state converts to
// float once, at the draw gather, and the float is never read back. Anything that round-trips
// through float mid-tick has reintroduced exactly the problem this file exists to remove.

/// Content → sim. Rounds to nearest; saturates rather than wrapping.
[[nodiscard]] Fx fxFromFloat(float value) noexcept;

/// Content → sim, for a magnitude.
[[nodiscard]] Mag magFromFloat(float value) noexcept;

/// Sim → renderer.
[[nodiscard]] float fxToFloat(Fx value) noexcept;
[[nodiscard]] float magToFloat(Mag value) noexcept;

/// Radians → binary radians, for content that authors an angle in radians, and its inverse
/// for the renderer.
[[nodiscard]] Brad bradFromRadians(float radians) noexcept;
[[nodiscard]] float radiansFromBrad(Brad angle) noexcept;

} // namespace rm::sim
