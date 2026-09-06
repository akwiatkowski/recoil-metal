#pragma once

// The script-object lifecycle seam: how a Lua-side handle reaches a native unit.
//
// Retail (WP-03, claims C-044 to C-046) resolves the receiver of every native call FRESH
// from the Lua object's `_c_object` pointer and never caches it. Two families of resolver
// exist: 54 of them raise "Game object has been destroyed" when the pointer is null, and 19
// lifecycle queries (`IsDestroyed`, `BeenDestroyed`, position reads in death callbacks)
// accept a destroyed object and return null instead. "Destroyed" itself has two moments —
// death queued (the object still exists, scripts may read its final state) and the pointer
// nulled in `~CScriptObject`.
//
// This header maps those two families onto `UnitStore::resolve`, which names the same two
// moments the tombstone store already has: health gone this tick, and handle released at
// the tick's end. Nothing here holds a slot across calls, so a handle that outlives its unit
// fails cleanly instead of following whoever inherits the slot. Nothing here changes tick
// order, the store's arrays, or the state hash.

#include "core/sim/IdPool.hpp"
#include "core/sim/UnitStore.hpp"

#include <expected>
#include <optional>
#include <string_view>

namespace rm::sim {

/// Retail's exact message for a native call on a nulled object (C-044), so a Lua host can
/// raise it verbatim and the corpus's own `pcall` sites recognise it.
inline constexpr std::string_view kDestroyedObjectError = "Game object has been destroyed";

/// The strict resolver family: the unit must be alive. Anything else is the retail error.
[[nodiscard]] inline std::expected<UnitIndex, std::string_view> requireAlive(
    const UnitStore& store, UnitId id) noexcept {
    const UnitStore::Resolved resolved = store.resolve(id);
    if (resolved.state != UnitStore::HandleState::Alive) {
        return std::unexpected{kDestroyedObjectError};
    }
    return resolved.slot;
}

/// The lifecycle family: a destroyed-but-not-yet-released unit is still readable, so death
/// callbacks can inspect its final position and owner. A released handle yields nothing.
[[nodiscard]] inline std::optional<UnitIndex> lifecycleSlot(const UnitStore& store,
                                                            UnitId id) noexcept {
    const UnitStore::Resolved resolved = store.resolve(id);
    if (resolved.state == UnitStore::HandleState::Stale) {
        return std::nullopt;
    }
    return resolved.slot;
}

/// Retail's `Entity:BeenDestroyed` (C-046): true once death is queued, not only once the
/// pointer is gone. `IsDestroyed` is the narrower question and is `Stale` alone.
[[nodiscard]] inline bool beenDestroyed(const UnitStore& store, UnitId id) noexcept {
    return store.resolve(id).state != UnitStore::HandleState::Alive;
}

} // namespace rm::sim
