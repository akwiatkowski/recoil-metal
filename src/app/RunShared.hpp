#pragma once

// The pieces of a run mode that BOTH the headless paths (`Run.mm`) and the
// windowed path (`WindowedSession.mm`) reach for: the `Session` unpacking
// macro and the four visibility gathers that decide what a frame shows.
//
// Internal to `src/app/` — `Run.hpp` is the modes' public face; nothing in
// here is a promise to any other layer. The helpers are `inline` because
// there is exactly one definition for two translation units, which is what
// the keyword exists for.

#include "app/Run.hpp"

namespace rm::app {

// The session's fields, named the way the code that used to be inside `main` named them. A
// Every name is `[[maybe_unused]]`: a benchmark reads no start positions and a screenshot
// opens no window, so a shared session is by nature partly unread by each of its users.
// `using`-style unpacking rather than a rename sweep: the bodies below are the same statements
// they were, which is what makes this move checkable by comparing a screenshot.
#define RM_UNPACK_SESSION(s)                                                                   \
    [[maybe_unused]] const int argc = (s).argc;                                                 \
    [[maybe_unused]] const char** argv = (s).argv;                                              \
    [[maybe_unused]] const LoadedMap* map = &(s).map;                                                           \
    [[maybe_unused]] UnitScene& units = (s).units;                                                              \
    [[maybe_unused]] const PropScene& props = (s).props;                                                        \
    [[maybe_unused]] PassabilitySet& passability = (s).passability;                                              \
    [[maybe_unused]] const rm::vfs::Vfs& content = (s).content;                                                 \
    [[maybe_unused]] const rm::Settings& settings = (s).settings;                                               \
    [[maybe_unused]] const rm::TerrainMesh& mesh = (s).mesh;                                                    \
    [[maybe_unused]] const std::span<const rm::mapinfo::StartPosition> starts = (s).starts;                      \
    [[maybe_unused]] std::vector<rm::Particle>& marchDust = (s).marchDust;                                      \
    [[maybe_unused]] const ShotOptions shot = (s).shot;                                                          \
    [[maybe_unused]] const BenchOptions bench = (s).bench;                                                       \
    [[maybe_unused]] const MarchOptions marchOptions = (s).march;                                                \
    [[maybe_unused]] const LookOptions look = (s).look;                                                          \
    [[maybe_unused]] const float focus = (s).focus;                                                              \
    [[maybe_unused]] const float animationTime = (s).animationTime;                                              \
    [[maybe_unused]] const std::size_t propInstances = (s).propInstances;

inline void appendVisibleWreckDecals(std::vector<rm::DecalVertex>& out,
                                     const UnitScene& units) {
    for (std::size_t i = 0; i + 2 < units.wreckDecals.size(); i += 3) {
        const rm::DecalVertex& first = units.wreckDecals[i];
        if (units.visibleToViewer(rm::sim::fxFromFloat(first.position[0]),
                                  rm::sim::fxFromFloat(first.position[2]))) {
            out.insert(out.end(), units.wreckDecals.begin() + static_cast<std::ptrdiff_t>(i),
                       units.wreckDecals.begin() + static_cast<std::ptrdiff_t>(i + 3));
        }
    }
}

inline void gatherVisibleProjectiles(std::vector<rm::sim::Projectile>& out,
                                     const UnitScene& units, float alpha = 0.0f) {
    out.clear();
    for (const rm::sim::Projectile& projectile : units.projectiles) {
        const float projectileAlpha =
            projectile.pendingImpact == rm::sim::ImpactType::Invalid ? alpha : 0.0f;
        const rm::sim::Fx x = rm::sim::fxFromFloat(
            rm::sim::fxToFloat(projectile.position[0])
            + rm::sim::fxToFloat(projectile.velocity[0]) * projectileAlpha);
        const rm::sim::Fx z = rm::sim::fxFromFloat(
            rm::sim::fxToFloat(projectile.position[2])
            + rm::sim::fxToFloat(projectile.velocity[2]) * projectileAlpha);
        if (units.visibleToViewer(x, z)) {
            out.push_back(projectile);
        }
    }
}

inline void appendVisibleParticles(std::vector<rm::Particle>& out,
                                   std::span<const rm::Particle> source,
                                   const UnitScene& units) {
    for (const rm::Particle& particle : source) {
        if (units.visibleToViewer(rm::sim::fxFromFloat(particle.origin[0]),
                                  rm::sim::fxFromFloat(particle.origin[2]))) {
            out.push_back(particle);
        }
    }
}

inline void gatherVisibleEvents(std::vector<rm::sim::Event>& out, const UnitScene& units) {
    out.clear();
    units.refreshViewerContacts();
    for (const rm::sim::Event& event : units.events.all()) {
        const bool matchEvent = event.kind == rm::sim::EventKind::TeamDefeated
                             || event.kind == rm::sim::EventKind::GameOver;
        const bool alliedUnitEvent = event.kind != rm::sim::EventKind::ProjectileImpact
                                  && units.alliedWithViewer(event.army);
        if (matchEvent || alliedUnitEvent || units.visibleToViewer(event.unit)
            || units.visibleToViewer(event.at[0], event.at[2])) {
            out.push_back(units.combatVisualEvent(event));
        }
    }
}

} // namespace rm::app
