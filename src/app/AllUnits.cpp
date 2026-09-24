#include "app/SceneBuild.hpp"

#include "core/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <vector>

namespace rm::app {
namespace {

// Two map squares beyond both collision discs leave a clear lane for the first move.
constexpr float kClearanceElmos = 2.0f * static_cast<float>(rm::kSquareSize);
// Four squares keep the edge structure's skirt and selection ring inside the map.
constexpr float kEdgeMarginElmos = 4.0f * static_cast<float>(rm::kSquareSize);
// Air parks nearby rather than on top of the land formation.
constexpr float kAirOffsetElmos = 32.0f * static_cast<float>(rm::kSquareSize);
// Eight 64-elmo route cells per fleet: ~1 km between opposing battle lines on SCMP_009.
constexpr int kFleetHalfGapCells = 8;
// The shipped Aeon Paragon; its income accrues to either faction's owning army.
constexpr std::string_view kParagon = "/units/XAB1401/XAB1401_unit.bp";

struct Occupied {
    float x;
    float z;
    float radius;
};

[[nodiscard]] float squaredDistance(float x, float z, float otherX, float otherZ) {
    const float dx = x - otherX;
    const float dz = z - otherZ;
    return dx * dx + dz * dz;
}

[[nodiscard]] bool roomFor(float x, float z, float radius, float width, float depth,
                           const std::vector<Occupied>& occupied) {
    const float margin = radius + kEdgeMarginElmos;
    if (x < margin || z < margin || x > width - margin || z > depth - margin) return false;
    return std::ranges::none_of(occupied, [=](const Occupied& other) {
        const float gap = radius + other.radius + kClearanceElmos;
        return squaredDistance(x, z, other.x, other.z) < gap * gap;
    });
}

[[nodiscard]] bool onOwnSide(float x, float z, const rm::mapinfo::StartPosition& home,
                             const rm::mapinfo::StartPosition& away) {
    return squaredDistance(x, z, home.x, home.z)
        < squaredDistance(x, z, away.x, away.z);
}

// Search outward from a group anchor in complete square rings. A naval mask
// replaces the land groups' own-side constraint: both fleets MUST share a single
// connected water body, including a deep-water path for the largest hull.
[[nodiscard]] std::optional<std::array<float, 3>> place(
    const rm::sim::PassabilityGrid& grid, float anchorX, float anchorZ, float radius,
    const rm::HeightField& field, const rm::mapinfo::StartPosition& home,
    const rm::mapinfo::StartPosition& away, const std::vector<Occupied>& occupied,
    std::span<const std::uint8_t> ocean = {}) {
    if (grid.cellsX <= 0 || grid.cellsZ <= 0) return std::nullopt;
    const int cx = grid.cellAtWorld(rm::sim::fxFromFloat(anchorX));
    const int cz = grid.cellAtWorld(rm::sim::fxFromFloat(anchorZ));
    const int reach = std::max(grid.cellsX, grid.cellsZ);
    for (int ring = 0; ring < reach; ++ring) {
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                const int xCell = cx + dx;
                const int zCell = cz + dz;
                if (!grid.passableAt(xCell, zCell)) continue;
                const std::size_t cell = static_cast<std::size_t>(zCell)
                    * static_cast<std::size_t>(grid.cellsX) + static_cast<std::size_t>(xCell);
                if (!ocean.empty() && (cell >= ocean.size() || ocean[cell] == 0)) continue;
                const float x = rm::sim::fxToFloat(grid.worldAtCellCentre(xCell));
                const float z = rm::sim::fxToFloat(grid.worldAtCellCentre(zCell));
                if ((ocean.empty() && !onOwnSide(x, z, home, away))
                    || !roomFor(x, z, radius, field.widthElmos(), field.depthElmos(), occupied)) {
                    continue;
                }
                // spawnUnit places flyers and hulls on their actual motion layer.
                return std::array<float, 3>{x, field.heightAtWorld(x, z), z};
            }
        }
    }
    return std::nullopt;
}

struct Ocean {
    std::vector<std::uint8_t> cells;
    std::size_t size = 0;
    float x = 0.0f;
    float z = 0.0f;
};

// A 'pool' is a four-neighbour connected component of the deepest hull's water
// grid. Take the largest one touching the map edge, not a nearby inland lake.
// Both armies draw from this SAME component, so every hull can reach the other.
[[nodiscard]] Ocean sideOcean(const rm::sim::PassabilityGrid& grid,
                              const rm::HeightField& field,
                              std::span<const rm::mapinfo::StartPosition> starts,
                              float largestRadius) {
    Ocean result;
    result.cells.resize(grid.divisor.size());
    std::vector<std::uint8_t> visited(grid.divisor.size());
    std::vector<std::size_t> queue;
    const auto index = [&grid](int x, int z) {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(grid.cellsX)
             + static_cast<std::size_t>(x);
    };
    for (int z = 0; z < grid.cellsZ; ++z) {
        for (int x = 0; x < grid.cellsX; ++x) {
            const std::size_t seed = index(x, z);
            if (visited[seed] != 0 || !grid.passableAt(x, z)) continue;
            queue.clear();
            queue.push_back(seed);
            visited[seed] = 1;
            bool touchesEdge = false;
            for (std::size_t front = 0; front < queue.size(); ++front) {
                const int cx = static_cast<int>(queue[front] % static_cast<std::size_t>(grid.cellsX));
                const int cz = static_cast<int>(queue[front] / static_cast<std::size_t>(grid.cellsX));
                touchesEdge |= cx == 0 || cz == 0 || cx == grid.cellsX - 1
                            || cz == grid.cellsZ - 1;
                for (const auto [dx, dz] : std::array<std::array<int, 2>, 4>{
                         {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
                    const int nx = cx + dx;
                    const int nz = cz + dz;
                    if (!grid.passableAt(nx, nz)) continue;
                    const std::size_t next = index(nx, nz);
                    if (visited[next] != 0) continue;
                    visited[next] = 1;
                    queue.push_back(next);
                }
            }
            if (!touchesEdge || queue.size() <= result.size) continue;
            result.size = queue.size();
            std::fill(result.cells.begin(), result.cells.end(), 0);
            for (const std::size_t cell : queue) result.cells[cell] = 1;
        }
    }
    if (result.size == 0) return result;

    // Start the battle at the part of this ocean closest to BOTH bases, with
    // enough open water to turn the largest hull before it receives an order.
    const float midpointX = (starts[0].x + starts[1].x) * 0.5f;
    const float midpointZ = (starts[0].z + starts[1].z) * 0.5f;
    float bestDistance = field.widthElmos() * field.widthElmos()
                       + field.depthElmos() * field.depthElmos();
    const float cellWidth = rm::sim::fxToFloat(grid.elmosPerCell);
    const int hullCells = static_cast<int>(std::ceil((largestRadius + kClearanceElmos) / cellWidth));
    bool found = false;
    for (int z = 0; z < grid.cellsZ; ++z) {
        for (int x = 0; x < grid.cellsX; ++x) {
            if (result.cells[index(x, z)] == 0) continue;
            bool open = true;
            for (int dz = -hullCells; dz <= hullCells && open; ++dz) {
                for (int dx = -hullCells; dx <= hullCells; ++dx) {
                    const int nx = x + dx;
                    const int nz = z + dz;
                    if (!grid.passableAt(nx, nz) || result.cells[index(nx, nz)] == 0) {
                        open = false;
                        break;
                    }
                }
            }
            if (!open) continue;
            const float wx = rm::sim::fxToFloat(grid.worldAtCellCentre(x));
            const float wz = rm::sim::fxToFloat(grid.worldAtCellCentre(z));
            const float d = squaredDistance(wx, wz, midpointX, midpointZ);
            if (d >= bestDistance) continue;
            bestDistance = d;
            result.x = wx;
            result.z = wz;
            found = true;
        }
    }
    if (!found) result.size = 0;
    return result;
}

} // namespace

void stageAllUnits(UnitScene& scene, const rm::HeightField& field,
                   std::span<const rm::mapinfo::StartPosition> starts,
                   const rm::vfs::Vfs& content, PassabilitySet& passability) {
    if (scene.armies.size() != 2 || starts.size() != 2) {
        throw std::invalid_argument{"--all-units requires exactly two armies and two starts"};
    }
    // Include both commanders and any scenario-placed units in spacing checks.
    std::vector<Occupied> occupied;
    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        if (!scene.store.slotAlive(slot)) continue;
        const auto* def = scene.catalog.def(scene.store.typeAt(slot));
        const auto& at = scene.store.transforms()[slot];
        occupied.push_back({rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.z),
                            def != nullptr ? def->collisionRadiusElmos : 0.0f});
    }

    // Paragons first, on the far edge of each side. Structures have no movement
    // grid, so place them on the dry-land movement domain.
    std::size_t paragons = 0;
    if (const auto type = ensureDrawableType(scene, content, kParagon)) {
        const auto* def = scene.catalog.def(*type);
        for (std::size_t army = 0; army < 2; ++army) {
            const auto& start = starts[army];
            const auto& land = passability.gridFor(
                rm::data::moveDefFor(rm::unitdef::MotionType::Land));
            const float edgeX = start.x < field.widthElmos() * 0.5f
                ? kEdgeMarginElmos + def->collisionRadiusElmos
                : field.widthElmos() - kEdgeMarginElmos - def->collisionRadiusElmos;
            const float edgeZ = start.z < field.depthElmos() * 0.5f
                ? field.depthElmos() - kEdgeMarginElmos - def->collisionRadiusElmos
                : kEdgeMarginElmos + def->collisionRadiusElmos;
            if (const auto at = place(land, edgeX, edgeZ, def->collisionRadiusElmos,
                                      field, start, starts[1 - army], occupied)) {
                if (spawnUnit(scene, content, field, kParagon, *at, scene.armies[army],
                              rm::Brad{})) {
                    occupied.push_back({(*at)[0], (*at)[2], def->collisionRadiusElmos});
                    ++paragons;
                }
            }
        }
    }

    // Resolve the naval blueprints first so the common water grid honours the
    // DEEPEST draft on either side. A shallower hull can also traverse that grid.
    float deepestDraft = 0.0f;
    float largestHull = 0.0f;
    std::array<std::size_t, 2> hullCounts{};
    for (std::size_t army = 0; army < 2; ++army) {
        for (const rm::data::RosterEntry& entry : scene.roster.entries()) {
            if (entry.faction != scene.armies[army].faction || !entry.mobile
                || entry.role == rm::unitdef::Role::Commander) continue;
            const auto type = ensureDrawableType(scene, content, entry.path());
            if (!type) continue;
            const auto* def = scene.catalog.def(*type);
            if (def->motion != rm::unitdef::MotionType::Water
                && def->motion != rm::unitdef::MotionType::SurfacingSub) continue;
            deepestDraft = std::max(deepestDraft,
                scene.moveDefForType[static_cast<std::size_t>(*type)].minWaterDepthElmos);
            largestHull = std::max(largestHull, def->collisionRadiusElmos);
            ++hullCounts[army];
        }
    }
    if (hullCounts[0] == 0 || hullCounts[1] == 0) {
        throw std::runtime_error{"--all-units needs naval blueprints for both factions"};
    }
    rm::data::MoveDef commonWater = rm::data::moveDefFor(rm::unitdef::MotionType::Water);
    commonWater.minWaterDepthElmos = deepestDraft;
    const rm::sim::PassabilityGrid& seaGrid = passability.gridFor(commonWater);
    const Ocean ocean = sideOcean(seaGrid, field, starts, largestHull);
    if (ocean.size == 0) {
        throw std::runtime_error{"--all-units needs one navigable side-map ocean for both fleets"};
    }
    const float separation = static_cast<float>(kFleetHalfGapCells)
                           * rm::sim::fxToFloat(seaGrid.elmosPerCell);
    const float deltaX = starts[1].x - starts[0].x;
    const float deltaZ = starts[1].z - starts[0].z;
    const float startDistance = std::hypot(deltaX, deltaZ);
    if (startDistance <= 0.0f) {
        throw std::invalid_argument{"--all-units requires distinct army starts"};
    }
    const float directionX = deltaX / startDistance;
    const float directionZ = deltaZ / startDistance;
    std::printf("all-units: shared ocean %zu cells, deepest draft %.0f, battle at %.0f,%.0f\n",
                ocean.size, static_cast<double>(deepestDraft),
                static_cast<double>(ocean.x), static_cast<double>(ocean.z));

    std::array<std::array<std::size_t, 3>, 2> counts{}; // land, air, sea
    std::array<std::size_t, 2> missing{};
    for (std::size_t army = 0; army < 2; ++army) {
        const auto& home = starts[army];
        const auto& away = starts[1 - army];
        const float inwardX = away.x >= home.x ? 1.0f : -1.0f;
        const float inwardZ = away.z >= home.z ? 1.0f : -1.0f;
        for (const rm::data::RosterEntry& entry : scene.roster.entries()) {
            // The two ACUs already stand at the map starts; do not spawn duplicates.
            if (entry.faction != scene.armies[army].faction || !entry.mobile
                || entry.role == rm::unitdef::Role::Commander) continue;
            const std::string path = entry.path();
            const auto type = ensureDrawableType(scene, content, path);
            if (!type) { ++missing[army]; continue; }
            const auto* def = scene.catalog.def(*type);
            const bool air = def->motion == rm::unitdef::MotionType::Air;
            const bool sea = def->motion == rm::unitdef::MotionType::Water
                          || def->motion == rm::unitdef::MotionType::SurfacingSub;
            const std::size_t group = sea ? 2u : air ? 1u : 0u;
            // Air has no routing grid; a few shipped mobile blueprints also
            // declare MotionType::None (XSC9002); park both on dry ground.
            const auto& grid = sea ? seaGrid
                : air || def->motion == rm::unitdef::MotionType::None
                    ? passability.gridFor(rm::data::moveDefFor(rm::unitdef::MotionType::Land))
                    : passability.gridFor(scene, *type);
            const float radius = def->collisionRadiusElmos;
            std::optional<std::array<float, 3>> at;
            if (sea) {
                const float sign = army == 0 ? -1.0f : 1.0f;
                at = place(grid, ocean.x + sign * directionX * separation,
                           ocean.z + sign * directionZ * separation, radius,
                           field, home, away, occupied, ocean.cells);
            } else {
                const float offset = air ? kAirOffsetElmos : 0.0f;
                at = place(grid, home.x + offset * inwardX, home.z + offset * inwardZ,
                           radius, field, home, away, occupied);
            }
            if (!at || !spawnUnit(scene, content, field, path, *at, scene.armies[army],
                                  rm::Brad{})) {
                rm::log::writef(rm::log::Level::Warn, "all-units",
                                "could not place %s for army %zu", entry.id.c_str(), army);
                ++missing[army];
                continue;
            }
            occupied.push_back({(*at)[0], (*at)[2], radius});
            ++counts[army][group];
        }
    }
    std::printf("all-units: %zu paragons; land %zu/%zu air %zu/%zu sea %zu/%zu; unplaced %zu/%zu\n",
                paragons, counts[0][0], counts[1][0], counts[0][1], counts[1][1],
                counts[0][2], counts[1][2], missing[0], missing[1]);
    if (paragons != scene.armies.size() || missing[0] != 0 || missing[1] != 0) {
        throw std::runtime_error{
            "--all-units needs room for every unit in its land group or shared ocean"};
    }
}

} // namespace rm::app
