#include "core/sim/Veterancy.hpp"
#include "core/sim/Enhancement.hpp"

#include "core/sim/Combat.hpp"  // positionOf
#include "core/sim/Events.hpp"
#include "core/sim/Health.hpp"
#include "core/sim/TickRate.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"
#include "core/unit/UnitDef.hpp"

#include <algorithm>
#include <span>

namespace rm::sim {

namespace {

/// The army a slot belongs to, or -1. Same helper shape the combat passes use.
[[nodiscard]] int armyOf(const UnitStore& store, UnitIndex slot) noexcept {
    const std::span<const MoveState> motion = store.motion();
    return slot < motion.size() ? motion[slot].armyIndex : -1;
}

} // namespace

bool creditKill(UnitStore& store, const UnitCatalog& catalog, UnitId killer,
                EventQueue* events) {
    // A STALE KILLER EARNS NOTHING, and that is the retail answer as well as the only one
    // available: `Health::lastHitBy` deliberately outlives its unit so a death can name who
    // did it, so the handle arriving here may belong to something that died on the same
    // tick. Resolving it through the generation is what tells the two apart.
    if (!store.alive(killer)) {
        return false;
    }
    const UnitIndex slot = killer.index;
    const std::span<Health> healths = store.health();
    if (slot >= healths.size()) {
        return false;
    }
    Health& health = healths[slot];

    // NOR DOES A KILLER AT ZERO HEALTH, and this guard is load-bearing rather than tidy.
    // `retireDead` walks slots in order, so a killer that died on the same tick but sits in a
    // later slot has not been retired yet: its handle is still live even though its health is
    // gone. Promoting it would raise its maximum and heal it by the increase — resurrecting a
    // unit that the very same loop was about to bury, in a way that depends on nothing but
    // slot order. Treating zero health as dead is also the honest reading: retirement is
    // bookkeeping, and death already happened when the health ran out.
    if (!health.alive()) {
        return false;
    }

    ++health.veterancy.kills;

    // AGAINST THIS TYPE'S OWN LADDER. Most combat units state one and it is far shorter than
    // `Game.VeteranDefault` — an interceptor promotes at 2 kills, not 25 — so using the
    // default for everything would make veterancy an order of magnitude too rare.
    const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
    const VeterancyThresholds& thresholds =
        def != nullptr ? def->veterancyKills : kVeterancyKillThresholds;

    const int earned = veterancyLevelFor(health.veterancy.kills, thresholds);
    if (earned <= health.veterancy.level) {
        return false;
    }
    health.veterancy.level = earned;

    // THE PROMOTION ITSELF (`C-028`, `C-029`). Recomputed from the blueprint maximum rather
    // than scaled from the current one, which is what stops the levels compounding — the
    // single most consequential detail in this feature.
    if (def != nullptr && def->health > Mag{}) {
        const Mag previousMaximum = health.maximum;
        health.maximum = veterancyMaxHealth(def->health + enhancementHealthAdd(store, catalog, slot), earned);
        if (health.maximum > previousMaximum) {
            // Retail heals by exactly the increase (`Buff.lua`'s `AdjustHealth(unit, val -
            // oldmax)`), so a unit promoted mid-fight is rewarded now rather than over the
            // next minute of regeneration.
            health.current += health.maximum - previousMaximum;
        } else {
            // The shrinking branch cannot be reached by promotion, since the multiplier only
            // rises. It is here because retail has it and because a blueprint-driven
            // threshold table would make it reachable; clamping is what it does.
            health.current = std::min(health.current, health.maximum);
        }
    }

    emit(events, Event{
                     .kind = EventKind::UnitVeteranPromoted,
                     .unit = killer,
                     .army = armyOf(store, slot),
                     .amount = Mag::fromInt(earned),
                     .at = positionOf(store.transforms()[slot]),
                 });
    return true;
}

void tickRegeneration(UnitStore& store, const UnitCatalog& catalog, TickRate rate) {
    const std::span<Health> healths = store.health();
    for (UnitIndex slot = 0; slot < healths.size(); ++slot) {
        if (!store.slotAlive(slot) || !healths[slot].alive()) {
            continue;
        }
        Health& health = healths[slot];
        if (health.current >= health.maximum) {
            continue;
        }

        // Base rate from the type, veteran bonus from the unit. The bonus is a per-second
        // figure converted here rather than stored per type for the obvious reason: it
        // depends on the unit's level, which the type does not know.
        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        const VeterancyRegen& ladder =
            def != nullptr ? def->veterancyRegenPerSecond : kVeterancyRegenPerSecond;
        const Mag perTick = catalog.rates(store.typeAt(slot)).regenPerTick
                            + veteranRegenPerTick(rate, health.veterancy.level, ladder)
                            + enhancementRegenPerTick(store, catalog, slot);
        if (perTick <= Mag{}) {
            continue;
        }
        health.current = std::min(health.maximum, health.current + perTick);
    }
}

} // namespace rm::sim
