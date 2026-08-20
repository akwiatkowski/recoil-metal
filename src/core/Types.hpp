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
// Deliberately absent: the fixed-point types (`FxRaw`, `FxWide`) and the authored-duration
// type (`Seconds`). Both are real and both are specified — see PLAN2.md §5.2 and §5.1 — but
// neither has a consumer yet, and a type with no consumer is a guess that reads as a
// decision. They land with the code that uses them.

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

/// A fingerprint of the sim's state at the end of a tick.
///
/// The whole determinism story rests on this being comparable: two runs of the same command
/// log agree tick for tick, or they diverge and this is what says on which tick. 64 bits so
/// that a collision is not the explanation anyone reaches for when two hashes match.
using StateHash = std::uint64_t;

} // namespace rm
