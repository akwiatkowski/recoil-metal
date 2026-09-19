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
    case EventKind::BeamFired:
        return "beam-fired";
    case EventKind::WeaponFired:
        return "weapon-fired";
    case EventKind::ProjectileImpact:
        return "projectile-impact";
    case EventKind::ShieldDamaged:
        return "shield-damaged";
    case EventKind::ShieldCollapsed:
        return "shield-collapsed";
    case EventKind::ShieldRestored:
        return "shield-restored";
    case EventKind::ConstructionStarted:
        return "construction-started";
    case EventKind::ConstructionFinished:
        return "construction-finished";
    case EventKind::ConstructionProgress:
        return "construction-progress";
    case EventKind::UnitVeteranPromoted:
        return "unit-veteran-promoted";
    case EventKind::TeamDefeated:
        return "team-defeated";
    case EventKind::GameOver:
        return "game-over";
    case EventKind::AnimTerrainCollision:
        return "anim-terrain-collision";
    case EventKind::AnimTerrainCollisionEnd:
        return "anim-terrain-collision-end";
    case EventKind::AnimCollision:
        return "anim-collision";
    case EventKind::EffectEmitted:
        return "effect-emitted";
    case EventKind::MotionHorz:
        return "motion-horz";
    case EventKind::MotionVert:
        return "motion-vert";
    case EventKind::MotionTurn:
        return "motion-turn";
    case EventKind::MotionState:
        return "motion-state";
    case EventKind::IntelChanged:
        return "intel-changed";
    case EventKind::DetectedBy:
        return "detected-by";
    case EventKind::StartBeingCaptured:
        return "start-being-captured";
    case EventKind::StartCapture:
        return "start-capture";
    case EventKind::StopCapture:
        return "stop-capture";
    case EventKind::StopBeingCaptured:
        return "stop-being-captured";
    case EventKind::Captured:
        return "captured";
    case EventKind::FailedCapture:
        return "failed-capture";
    case EventKind::FailedBeingCaptured:
        return "failed-being-captured";
    case EventKind::ArmyStatTriggered:
        return "army-stat-triggered";
    case EventKind::UnitCapLimitReached:
        return "unit-cap-limit-reached";
    }
    return "unknown";
}

bool operator==(const Event& a, const Event& b) noexcept {
    return a.kind == b.kind && a.unit == b.unit && a.instigator == b.instigator && a.builder == b.builder
           && a.army == b.army && a.amount == b.amount && a.at == b.at
           && a.impactType == b.impactType;
}

std::size_t EventQueue::count(EventKind kind) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(),
                      [kind](const Event& event) { return event.kind == kind; }));
}

} // namespace rm::sim
