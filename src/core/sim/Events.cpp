#include "core/sim/Events.hpp"

#include <algorithm>

namespace rm::sim {

std::string_view eventKindName(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::UnitCreated:
        return "unit-created";
    case EventKind::UnitFinished:
        return "unit-finished";
    case EventKind::UnitDamaged:
        return "unit-damaged";
    case EventKind::UnitDestroyed:
        return "unit-destroyed";
    case EventKind::WeaponFired:
        return "weapon-fired";
    case EventKind::ProjectileImpact:
        return "projectile-impact";
    case EventKind::ConstructionStarted:
        return "construction-started";
    case EventKind::ConstructionFinished:
        return "construction-finished";
    case EventKind::TeamDefeated:
        return "team-defeated";
    case EventKind::GameOver:
        return "game-over";
    }
    return "unknown";
}

bool operator==(const Event& a, const Event& b) noexcept {
    return a.kind == b.kind && a.unit == b.unit && a.instigator == b.instigator
           && a.army == b.army && a.amount == b.amount && a.at == b.at;
}

std::size_t EventQueue::count(EventKind kind) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(),
                      [kind](const Event& event) { return event.kind == kind; }));
}

} // namespace rm::sim
