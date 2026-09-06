#include "core/scene/ProjectileTrails.hpp"

#include <algorithm>
#include <cmath>

namespace rm {
namespace {

[[nodiscard]] std::array<float, 3> toFloat(const std::array<sim::Fx, 3>& at) {
    return {sim::fxToFloat(at[0]), sim::fxToFloat(at[1]), sim::fxToFloat(at[2])};
}

[[nodiscard]] float distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dx = b[0] - a[0];
    const float dy = b[1] - a[1];
    const float dz = b[2] - a[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// The ribbon materials of a definition, and the longest TrailLength among them.
struct Ribbons {
    std::vector<std::uint32_t> ids;
    float length = 0;
};

[[nodiscard]] Ribbons ribbonsOf(const WeaponVisuals& visuals, std::string_view key) {
    Ribbons found;
    for (const auto id : visuals.find(key)) {
        const auto& material = visuals.materials[id];
        if (!material.ribbon || material.length <= 0) continue;
        found.ids.push_back(id);
        found.length = std::max(found.length, material.length);
    }
    return found;
}

/// Drops points that can no longer contribute: everything beyond the visible window
/// measured back from the real head, keeping one point past it for the clipped segment.
void trim(ProjectileTrail& trail) {
    const float window = trail.drained + trail.length;
    float walked = 0;
    for (std::size_t i = trail.points.size(); i-- > 1;) {
        walked += distance(trail.points[i], trail.points[i - 1]);
        if (walked > window) {
            trail.points.erase(trail.points.begin(),
                               trail.points.begin() + static_cast<std::ptrdiff_t>(i - 1));
            return;
        }
    }
}

} // namespace

void ProjectileTrails::update(std::span<sim::Projectile> shots, const WeaponVisuals& visuals,
                              float seconds) {
    std::vector<bool> seen(trails_.size(), false);
    for (sim::Projectile& shot : shots) {
        const Ribbons ribbons = ribbonsOf(visuals, shot.visualId);
        if (ribbons.ids.empty()) continue;

        const std::array<float, 3> at = toFloat(shot.position);
        const std::array<float, 3> velocity = toFloat(shot.velocity);
        const float perTick = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]
                                        + velocity[2] * velocity[2]);
        const float speed = seconds > 0 ? perTick / seconds : 0;

        if (shot.visualSerial == 0) {
            shot.visualSerial = nextSerial_++;
            ProjectileTrail trail{.serial = shot.visualSerial, .key = shot.visualId,
                                  .velocity = velocity, .speed = speed, .length = ribbons.length};
            // The muzzle first, so the ribbon reaches back to where the shot came from
            // rather than starting one tick downrange.
            const std::array<float, 3> origin = toFloat(shot.visualOrigin);
            if (distance(origin, at) > 0) trail.points.push_back(origin);
            trail.points.push_back(at);
            trails_.push_back(std::move(trail));
            seen.push_back(true);
            continue;
        }
        const auto found = std::ranges::find(trails_, shot.visualSerial, &ProjectileTrail::serial);
        if (found == trails_.end()) continue; // its trail already drained away
        seen[static_cast<std::size_t>(found - trails_.begin())] = true;
        if (distance(found->points.back(), at) > 0) found->points.push_back(at);
        found->velocity = velocity;
        found->speed = speed;
        trim(*found);
    }

    for (std::size_t i = 0; i < trails_.size(); ++i) {
        if (seen[i]) continue;
        ProjectileTrail& trail = trails_[i];
        trail.alive = false;
        trail.drained += trail.speed * seconds;
        trim(trail);
    }
    // Gone once the virtual head has travelled the whole recorded path: nothing is inside
    // the window any more. A trail whose shot stood still drains nothing and would stay
    // forever, so a zero speed retires it at once.
    std::erase_if(trails_, [](const ProjectileTrail& trail) {
        if (trail.alive) return false;
        if (trail.speed <= 0) return true;
        float total = 0;
        for (std::size_t i = 1; i < trail.points.size(); ++i) {
            total += distance(trail.points[i - 1], trail.points[i]);
        }
        return trail.drained >= total;
    });
}

void ProjectileTrails::append(std::vector<Particle>& out, const WeaponVisuals& visuals,
                              float alpha, float elmosPerPoint, const Visible& visible) const {
    for (const ProjectileTrail& trail : trails_) {
        const Ribbons ribbons = ribbonsOf(visuals, trail.key);
        if (ribbons.ids.empty() || trail.points.empty() || trail.length <= 0) continue;

        // The head, extrapolated by the frame's tick fraction while the shot is alive.
        std::array<float, 3> head = trail.points.back();
        if (trail.alive) {
            for (std::size_t axis = 0; axis < 3; ++axis) head[axis] += trail.velocity[axis] * alpha;
        }
        if (visible && !visible(head)) continue;

        // Walk back from the head. `d` is the trail-relative distance of the near end of
        // the current segment, starting at how far the ribbon has drained past the head.
        float d = trail.drained;
        std::array<float, 3> near = head;
        for (std::size_t i = trail.points.size(); i-- > 0;) {
            std::array<float, 3> far = trail.points[i];
            float span = distance(near, far);
            if (span <= 0) continue;
            if (d >= trail.length) break;
            if (d + span > trail.length) {
                // Clip to the authored length: the far end lands exactly on the tail.
                const float keep = (trail.length - d) / span;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    far[axis] = near[axis] + (far[axis] - near[axis]) * keep;
                }
                span = trail.length - d;
            }
            std::array<float, 3> axis{near[0] - far[0], near[1] - far[1], near[2] - far[2]};
            for (auto& value : axis) value /= span;
            const std::array<float, 2> range{(d + span) / trail.length, d / trail.length};
            for (const auto id : ribbons.ids) {
                const auto& material = visuals.materials[id];
                out.push_back(Particle{
                    .origin = far, .age = 0.0f, .velocity = {}, .lifetime = 1.0f,
                    .colour = {1, 1, 1, 0}, .size = std::max(material.width, elmosPerPoint),
                    .axis = axis, .length = span, .material = id, .flags = 2u,
                    .trailRange = range,
                });
            }
            d += span;
            near = trail.points[i];
        }
    }
}

} // namespace rm
