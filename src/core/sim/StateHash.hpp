#pragma once

#include "core/Types.hpp"
#include "core/sim/Skirmish.hpp"

#include <span>

namespace rm::sim {

// A fingerprint of everything a match IS at the end of a tick.
//
// WHY THIS EXISTS. The project's success criterion is that the same command log produces
// the same match, on this machine and on another architecture (PLAN2.md §1.3). "The same
// match" needs a definition you can compare in one comparison, and this is it. Everything
// else the determinism story rests on — the replay, the divergence harness, the
// cross-platform proof — is this function plus bookkeeping.
//
// It is also the test oracle the analysis says a reimplementation normally cannot have:
// `16-new-engine-feasibility.md §4` observes that black-box parity projects have no oracle,
// so every regression test becomes "run both and diff". We are not chasing anyone's
// behaviour, so our sim is the only authority on what it did last time — which makes a
// hash of its state a complete oracle, for free.
//
// WHAT IS DELIBERATELY NOT HASHED. Presentation. `UnitInstance` is a GPU layout as much as
// it is sim state (PLAN2.md §1.2), and two of its fields belong to the renderer:
// `animationPhase`, which the caller advances from distance walked, and `teamColour`, which
// is a palette lookup fixed at spawn. Hashing either would make the fingerprint disagree
// between two runs that played the identical match and drew it differently — the exact
// false positive that makes a divergence checker useless. When P7 splits the struct, these
// two stop being reachable from here at all and this paragraph becomes a note about
// history.
//
// HOW IT IS FED. Field by field, never as a block of struct bytes. Padding between members
// is unspecified, so a `memcpy` of a struct hashes whatever the compiler left in the gaps —
// which turns a padding decision into a false divergence between two builds of the same
// source. Counts are fed too, so that losing a unit changes the hash even when every
// surviving unit matches.
//
// Floats are fed as their bit patterns, on purpose. Determinism means bit-identical or not
// identical; a hash that compared them with a tolerance would hide precisely the drift it
// exists to find.

/// Hashes the whole match: every unit's position, motion and health, every army, economy,
/// projectile and construction.
///
/// Order-sensitive, because the order IS state — two units swapping slots is a different
/// match, and a sim that reordered them would be a sim whose behaviour depends on
/// iteration order, which is the thing determinism forbids.
[[nodiscard]] StateHash hashMatch(std::span<const SkirmishGroup> groups, const Match& match);

} // namespace rm::sim
