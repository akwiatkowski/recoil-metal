#pragma once

#include <cstdint>

// The widths the sim counts in, in one place.
//
// WHY THIS FILE EXISTS. Every one of these is a number whose range is a guess about how big
// a match gets, and a guess is exactly the thing you want to be able to change without
// reading the code that depends on it. A bare `std::uint32_t` spelled out at forty call
// sites is forty edits and a chance to miss one; an alias is a line.
//
// THE RULE: nothing in the sim's own state uses a bare `int`, `unsigned`, or `std::size_t`.
// `std::size_t` in particular is a container index — it is the right type for walking a
// vector and the wrong type for a thing the sim remembers, because its width is the
// platform's opinion rather than ours, and the replay hash below has to mean the same on
// both platforms we intend to run on.
//
// CHANGING A WIDTH CHANGES THE REPLAY HASH, so the replay format carries a version and a
// fingerprint of these types. Bumping `UnitIndex` to 64-bit invalidates old logs — by
// design, and loudly, rather than silently comparing hashes that were never comparable.
//
// Deliberately absent: the authored-duration type (`Seconds`). Real and specified (PLAN2.md
// §5.1) but with no consumer yet, and a type with no consumer is a guess that reads as a
// decision. It lands with the code that uses it (P2.3).

namespace rm {

/// A unit's slot in the store. 4.3e9 units is far past anything a match will hold; the
/// alias is here so the ceiling is a line to change rather than a search.
using UnitIndex = std::uint32_t;

/// A slot in the shared object space — units, features and projectiles together, so that a
/// weapon can target any of them and collision can treat them alike without asking which
/// kind it has.
using ObjectIndex = std::uint32_t;

/// How many times a slot has been reused. Paired with an index to make a handle: a stale
/// handle names a generation that has moved on, so it fails to resolve rather than quietly
/// naming whoever inherited the slot. Wrapping is harmless — it would take 4.3e9 deaths in
/// the same slot to alias, and by then the handle in question is long dead.
using Generation = std::uint32_t;

/// Who owns a unit and banks its resources. See PLAN2.md §6.3 for why this is separate from
/// a player and from an alliance; 65k of each is beyond generous.
using TeamIndex = std::uint16_t;

/// A participant — human or script. Several may drive one team.
using PlayerIndex = std::uint16_t;

/// Who wins together, and later who shares vision.
using AllianceIndex = std::uint16_t;

/// A unit type in the catalog. 606 Forged Alliance blueprints and 969 Beyond All Reason
/// unit defs; 16 bits is ample and keeps the per-unit record small.
using UnitTypeIndex = std::uint16_t;

/// WHAT A UNIT IS MADE OF, for the purpose of deciding how much a weapon hurts it. An index
/// into an `ArmorRegistry` (`core/unit/Armor.hpp`), which is content — so this is the same
/// kind of thing as `UnitTypeIndex`, and it lives here for the same reason.
///
/// **8 bits, and that is a real ceiling rather than a generous one**, so it is stated here
/// where someone changing it will look. Forged Alliance has 8 distinct classes
/// (`14-blueprint-census.md §8.7`, measured over all 604 blueprints that state one) and
/// Recoil's own `armordefs.lua` is conventionally a similar size, so 255 is roughly thirty
/// times the largest real corpus. What buys the small width is that `DamageProfile` stores an
/// override as `{ArmorClass, Mag}` and is copied per shot — a 16-bit class would grow the
/// struct by its own padding for range nothing has ever needed.
///
/// Zero is `default` ALWAYS — the class a unit gets when its blueprint states none, and the
/// class a damage lookup falls back to. Recoil makes the same reservation
/// (`DamageArrayHandler.cpp:43-45`, index 0 inserted at the front of the sorted key list).
using ArmorClass = std::uint8_t;

/// Which tick it is, counted from the match's first. 64-bit from the start: it costs
/// nothing next to everything else a unit carries, and replays are concatenated — a 32-bit
/// counter would wrap after about two and a half years of match time at the fast end of the
/// permitted rate, which is the kind of limit that is only ever discovered the hard way.
using TickIndex = std::uint64_t;

/// A duration in ticks. Always DERIVED from a rate and an authored duration in seconds,
/// never written as a literal — see PLAN2.md §5.1. A tick count in a constant is a value
/// that is only correct at one rate, with the rate recorded in a comment instead of in the
/// arithmetic, and that is the bug the rule exists to prevent.
using TickCount = std::uint32_t;

// --- Fixed point (PLAN2.md §5.2, D1) --------------------------------------------------
//
// **This diverges from PLAN2 §5.2's stated Q16.16, on measurement.** The plan specified
// `FxRaw = int32` with 16 fractional bits, giving a range of ±32,768. Two facts from the
// shipped corpus put values outside it:
//
//   - **Three shipped maps are 32,768 × 32,768 elmos** — SCMP_029, SCMP_030 and X1MP_012, the
//     81 × 81 km size, at 4096 × 4096 squares of 8 elmos (`map/Scmap.hpp`). The far corner of
//     those maps is exactly the largest value Q16.16 can hold, so a unit standing there is
//     not representable and one a metre further is negative. Measured by loading all 61 maps
//     in the corpus.
//   - **`BuildCostEnergy` reaches 10,008,000** (XSB2401) and **`MaxHealth` 5,000,000**
//     (XSC9010), against Q16.16's ±32,768. Measured across all 568 `*_unit.bp` blueprints in
//     `units.scd`.
//
// So geometry needs more integer range than 16 bits, and magnitudes need far more than any
// 32-bit fixed-point layout can give. Hence two types — and, importantly, **both use the same
// number of fractional bits**, so converting between them is a widening or a narrowing and
// never a rescale. Two different fractional counts is precisely the mistake
// `recoil-engine-map.md §2` records in Recoil's own content format, where `speed` is per
// second and `maxVelocity` per frame in the same table; every conversion becomes a place to
// get the scale wrong.

/// How many bits of a fixed-point value are fractional. **14**, not 16.
///
/// The trade against Q16.16 is resolution: 1/16,384 ≈ 6.1e-5 elmos instead of 1/65,536. That
/// is four orders of magnitude below the smallest per-tick movement in the corpus — the
/// slowest units move a few elmos a second, so ~0.1 elmo in a tick at the fast end of the
/// permitted rate (PLAN2 §5.1's own note) — so the precision is bought with nothing that
/// matters, and it buys the ability to stand in the corner of the biggest shipped map.
inline constexpr int kFxFractionalBits = 14;

/// Geometry: position, velocity, heading, radius, speed. **Q18.14** — range ±131,072 elmos,
/// four times the largest shipped map, resolution 1/16,384 of an elmo.
using FxRaw = std::int32_t;

/// The widening type every fixed-point multiply and divide goes through.
///
/// A multiply is `(a * b) >> kFxFractionalBits`, and `a * b` overflows 32 bits long before
/// either operand does: two coordinates on the biggest map are ~5.4e8 raw each, so their
/// product is ~2.9e17 — comfortable in 64 bits, catastrophic in 32. Every operation that can
/// widen, does.
using FxWide = std::int64_t;

/// Magnitudes: health, resources, costs, cumulative damage. **Q50.14** — range ±5.6e14, which
/// is seven orders of magnitude past the largest value in the corpus.
///
/// Separate from `FxRaw` rather than making everything 64-bit: position, velocity and heading
/// are what the sim has thousands of and hashes every tick, and doubling their width doubles
/// the state the fingerprint walks for range no geometry needs.
using MagRaw = std::int64_t;

/// An angle, as a fraction of a full turn: 65,536 **binary radians** to the circle.
///
/// NOT fixed-point radians, and the reason is determinism rather than taste. An angle in
/// radians has to be wrapped into range, and wrapping means a modulo by 2π — an irrational
/// number that no fixed-point type holds exactly, so the wrap introduces an error that
/// accumulates over a match. A turn split into 2^16 wraps by unsigned overflow: exact, free,
/// and impossible to get wrong. Recoil reaches the same conclusion — `turnRate` in its unit
/// defs is in 65,536-units per frame (`recoil-engine-map.md §2`).
///
/// Resolution is 360/65,536 ≈ 0.0055°, about a tenth of the tightest firing tolerance in the
/// corpus (2°, `Weapon.hpp`).
using Brad = std::uint16_t;

/// A fingerprint of the sim's state at the end of a tick.
///
/// The whole determinism story rests on this being comparable: two runs of the same command
/// log agree tick for tick, or they diverge and this is what says on which tick. 64 bits so
/// that a collision is not the explanation anyone reaches for when two hashes match.
using StateHash = std::uint64_t;

} // namespace rm
