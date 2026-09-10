#pragma once

#include "core/unit/UnitDef.hpp"

#include <cstdint>
#include <string_view>

namespace rm::unitdef {

// What a unit IS FOR, inferred from the categories its blueprint declares.
//
// WHY THIS EXISTS (PLAN2.md §7 P3.1). Everything above the sim needs to talk about kinds of
// unit rather than about blueprint ids. The scripted opponent currently names four paths —
// `/units/UEB1103/UEB1103_unit.bp` and three siblings — hardcoded in a C++ header, which means
// it can only ever play UEF, and changing the build order means editing and recompiling. Both
// reference engines keep ZERO game rules in C++ (§1.1). A role is the first step out: an order
// that asks for "an extractor" works for four factions, and a data file can name one.
//
// THE VOCABULARY IS NOT INVENTED HERE. It is `07-ai-and-gamesetup.md §4.4`'s `far_role` list,
// chosen there to be a superset of what BAR's `ai_simpleai.lua` infers and a subset of
// CircuitAI's compiled enum (`src/circuit/unit/CircuitDef.h:49-59`) — so the same tag serves
// both, and neither has to be hand-written later.
//
// PLAN2 §7 P3.1 quotes that list with three omissions — `antinaval`, `bomber` and `sub`. The
// report's full twenty-one are used here, because the report is the cited source and the three
// missing ones are real distinctions in the corpus (a torpedo bomber is not an interceptor).
//
// A UNIT HAS ONE ROLE, which is a simplification and a deliberate one. A commander is a
// builder, an energy producer and a mass producer at once; a factory is a builder that cannot
// move. The single role is the unit's PRIMARY purpose, resolved by the precedence in
// `roleOf`, and the individual capabilities stay readable on the def (`isBuilder()`,
// `producesMassPerSecond`). Where a caller wants "can this build things" it should ask that
// rather than compare roles.

/// What a unit is for. See `07 §4.4`.
enum class Role : std::uint8_t {
    /// Nothing recognised. An honest answer for content this engine has not learned to read —
    /// not an error. 34 of the 568 shipped blueprints land here, measured.
    Unknown,

    /// Map decoration and campaign scenery: `CIVILIAN`, and usually `BENIGN` with it. 146 of
    /// the 568 shipped units are these.
    ///
    /// A ROLE OF ITS OWN rather than `Unknown`, and the distinction earns itself twice. It
    /// separates "we could not classify this" from "this is a house", which is the difference
    /// between a bug and a fact — before this existed, civilian buildings were four fifths of
    /// the unclassified pile and made the tally look like a broken classifier. And a build
    /// order must never pick one: asking for a `defence` and getting a civilian bunker is a
    /// mistake `Unknown` could not have prevented, because `Unknown` is where the real
    /// failures live too.
    Civilian,

    Commander,
    Builder,
    Factory,
    Extractor,
    Energy,
    Storage,
    Defence,
    Raider,
    Assault,
    Artillery,
    AntiAir,
    AntiNaval,
    Air,
    Bomber,
    Naval,
    Sub,
    Scout,
    Transport,
    Shield,
    Radar,
    Experimental,
};

/// The spelling a data file uses, so a build order can name a role without a table of its own.
[[nodiscard]] std::string_view roleName(Role role) noexcept;

/// The inverse, for reading a data file. Nothing for a name this engine does not know, which
/// is what lets a newer data file fail loudly on an older binary.
[[nodiscard]] std::optional<Role> roleFromName(std::string_view name) noexcept;

/// Which tech tier, 1 to 4. Zero for something that declares none.
///
/// `TECH1..TECH3` and `EXPERIMENTAL` are categories like any other; the tier is separate from
/// the role because a T1 and a T3 tank are the same KIND of thing and a build order cares
/// about both facts independently.
[[nodiscard]] int techOf(const UnitDef& def) noexcept;

/// A unit's primary purpose.
///
/// PRECEDENCE, and it is the whole design — the categories overlap heavily, so the order in
/// which they are tested is what decides the answer:
///
///   1. `COMMAND` wins over everything. An ACU is a builder and an economy and a weapon
///      platform, and it is a commander first: losing it ends the match.
///   2. `EXPERIMENTAL` next, because an experimental's role in a build order is "the big one"
///      regardless of whether it walks or flies.
///   3. `CIVILIAN` next. Scenery is scenery even when it has a gun, and a campaign map's
///      defended village must not enter a build order.
///   4. The ECONOMIC kinds — extractor, energy, storage — before the military ones, because a
///      mass extractor with a token gun is still an extractor.
///   5. `FACTORY` before an explicit `ENGINEER`/`CONSTRUCTION` builder. A bare BuildRate is a
///      capability fallback after military roles, because the Mantis carries one for repair.
///   6. Then the military kinds, from the most specific outward. An explicit `SCOUT` beats
///      the `RADAR` it commonly carries; the generic `INTELLIGENCE` tag is not itself a role.
///
/// Reading the precedence top to bottom is reading the classifier; there is no second place
/// where it is decided.
[[nodiscard]] Role roleOf(const UnitDef& def) noexcept;

/// Whether a unit both moves and fights without being told to: a speed and at least one
/// weapon that acquires targets on its own.
///
/// This is the box-select class, not a role: BAR's drag-select prefers mobile combat units
/// over the engineers and buildings caught in the same box, and the idle-combat hotkey (C)
/// asks the same question. A point-defence turret is armed but never goes anywhere; a field
/// engineer goes everywhere but holds no gun — neither answers yes.
[[nodiscard]] bool isMobileCombat(const UnitDef& def) noexcept;

} // namespace rm::unitdef
