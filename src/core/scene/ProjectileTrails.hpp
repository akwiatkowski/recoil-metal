#pragma once

// Ribbon trails: the original TrailBlueprints, drawn over each shot's recorded path.
//
// A PolyTrail in Supreme Commander is a ribbon of the projectile's recent positions,
// `TrailLength` ogrids long, textured once along its length with the ramp's left edge at
// the head and its right edge at the tail. ProjectileFx draws a projectile as a straight
// strip a tick long; that cannot show an arc bending, so trail materials are left to this
// class, which remembers where each shot has been.
//
// PRESENTATION STATE ONLY. The sim compacts its projectile list on impact and has no
// identity to offer across ticks, so the first time a shot is seen it is given a serial in
// a cosmetic field the state hash never reads. Nothing here is consulted by the sim.
//
// Two clocks, like ProjectileFx: `update` runs once per TICK and records the path;
// `append` runs per FRAME, extrapolates the head by the frame's tick fraction and emits
// one strip per segment into a scratch list that never ages.

#include "core/scene/Particles.hpp"
#include "core/scene/WeaponVisuals.hpp"
#include "core/sim/Combat.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace rm {

/// One shot's recorded path, kept for as long as any of its ribbon is left to draw.
struct ProjectileTrail {
    std::uint32_t serial = 0;
    std::string key;                          ///< the projectile blueprint path
    std::vector<std::array<float, 3>> points; ///< oldest first, the head last
    std::array<float, 3> velocity{};          ///< elmos per tick at the last live update
    float speed = 0;                          ///< elmos per second at the last live update
    float length = 0;                         ///< longest authored TrailLength of its ribbons, elmos
    bool alive = true;
    /// After impact the head stops but the ribbon keeps moving: this is how far the virtual
    /// head has travelled past the real one, so the tail slides out at the shot's own speed.
    float drained = 0;
};

class ProjectileTrails {
public:
    /// Records this tick's positions. Shots whose visuals include a ribbon material receive
    /// a serial on first sight; shots that disappeared keep draining until nothing is left.
    void update(std::span<sim::Projectile> shots, const WeaponVisuals& visuals, float seconds);

    /// Says whether a world position is visible to the viewer; a trail whose head is not is
    /// skipped entirely, so a shot in the fog does not draw its history.
    using Visible = std::function<bool(const std::array<float, 3>&)>;

    /// Emits one strip per segment inside the authored length, head first. `alpha` is the
    /// frame's fraction of a tick, used to extrapolate a live head; `elmosPerPoint` floors
    /// the width the way bolts are floored so a thin ribbon still reads from altitude.
    void append(std::vector<Particle>& out, const WeaponVisuals& visuals, float alpha,
                float elmosPerPoint, const Visible& visible = {}) const;

    [[nodiscard]] std::size_t size() const noexcept { return trails_.size(); }

private:
    std::vector<ProjectileTrail> trails_;
    std::uint32_t nextSerial_ = 1;
};

} // namespace rm
