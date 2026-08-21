#include "core/sim/Intel.hpp"

#include "core/sim/Terrain.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Transform.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace rm::sim {

namespace {

/// Added to every square's height before its angle is taken — Recoil's `LOS_BONUS_HEIGHT`
/// (`LosMap.cpp:16`), five elmos of slack that stops a unit being blinded by a ripple in
/// the ground it is standing on. Elmos, like the heights it is added to.
constexpr std::int32_t kSightBonusElmos = 5;

/// An angle so low nothing can fall below it. Recoil's -1e7 sentinel; here it is the
/// fixed-point type's floor, which cannot be reached by a real height over a real distance.
const Fx kNoAngle = Fx::fromRaw(std::numeric_limits<FxRaw>::min());

/// Calls `row(halfWidth, z)` once for each line of a filled disc of `radius` squares.
///
/// Recoil's `MidpointCircleAlgoPerLine` (`LosMap.cpp:74-103`), transcribed. Integer
/// arithmetic throughout — no square roots, no trigonometry, hence nothing that could
/// differ between two machines.
///
/// THE `x != y` GUARD IS LOad-BEARING, not a micro-optimisation: without it the diagonal
/// where the two octants meet emits its row twice, and every count in that row would be
/// double. `covered` would survive that (removal doubles too) and every reader of the
/// number would not.
template <typename Row>
void midpointCircleRows(std::int32_t radius, const Row& row) {
    std::int32_t x = radius;
    std::int32_t z = 0;
    std::int32_t decisionOver2 = 1 - x;

    while (x >= z) {
        row(x, z);
        if (z != 0) {
            row(x, -z);
        }

        if (decisionOver2 <= 0) {
            ++z;
            decisionOver2 += 2 * z + 1;
        } else {
            if (x != z) {
                row(z, x);
                if (x != 0) {
                    row(z, -x);
                }
            }
            ++z;
            --x;
            decisionOver2 += 2 * (z - x) + 1;
        }
    }
}

} // namespace

IntelGrid::IntelGrid(Fx widthElmos, Fx depthElmos, int mipLevel) {
    assert(mipLevel >= 0 && mipLevel < 16);
    squareElmos_ = kElmosPerSquare << mipLevel;

    // FLOOR, so a map that is not a whole number of squares across loses its last partial
    // one rather than gaining a square that is mostly off the map. At least one either way:
    // a grid with no squares would make every coverage query answer "off the map", which
    // reads as an intel system that is switched off rather than one that is misconfigured.
    squaresX_ = std::max(1, widthElmos.floorToInt() / squareElmos_);
    squaresZ_ = std::max(1, depthElmos.floorToInt() / squareElmos_);

    counts_.assign(static_cast<std::size_t>(squaresX_) * static_cast<std::size_t>(squaresZ_),
                   std::uint16_t{0});
}

std::int32_t IntelGrid::squareAt(Fx x, Fx z) const noexcept {
    // Negative coordinates are off the map, and the check has to come BEFORE the division:
    // `-1 / 8` is 0 in C++, so a unit one elmo past the western border would otherwise
    // land in column zero and see from inside the map.
    if (x < kFxZero || z < kFxZero) {
        return kNoSquare;
    }

    const std::int32_t sx = x.floorToInt() / squareElmos_;
    const std::int32_t sz = z.floorToInt() / squareElmos_;
    if (sx >= squaresX_ || sz >= squaresZ_) {
        return kNoSquare;
    }
    return sz * squaresX_ + sx;
}

void IntelGrid::add(std::span<const std::int32_t> squares) noexcept {
    for (const std::int32_t square : squares) {
        ++counts_[static_cast<std::size_t>(square)];
    }
}

void IntelGrid::remove(std::span<const std::int32_t> squares) noexcept {
    for (const std::int32_t square : squares) {
        // An assert rather than a clamp. A count that reaches zero with a withdrawal still
        // to come means some emitter removed a shape it never added — which is a bookkeeping
        // bug in the caller, and clamping it here would hide the cause and leave the grid
        // quietly wrong for the rest of the match.
        assert(counts_[static_cast<std::size_t>(square)] > 0);
        --counts_[static_cast<std::size_t>(square)];
    }
}

namespace {

/// One ray: the offsets it visits, nearest first, in the quarter turn x >= 0, z >= 0.
using Ray = std::vector<std::pair<std::int32_t, std::int32_t>>;

/// The offsets a zero-width line from the origin to (toX, toZ) passes through.
///
/// Recoil's `GetRay` (`LosMap.cpp:283`) with its float slope replaced by an integer
/// rounding division — `(a * step * 2 + span) / (2 * span)` is `round(a * step / span)` for
/// non-negative inputs, and unlike the float it cannot round differently on another
/// machine. The origin itself is not included; the caller always sees where it stands.
void buildRay(std::int32_t toX, std::int32_t toZ, Ray& ray) {
    ray.clear();
    const auto rounded = [](std::int32_t a, std::int32_t step, std::int32_t span) {
        return (a * step * 2 + span) / (2 * span);
    };

    if (toX > toZ) {
        for (std::int32_t x = 1; x <= toX; ++x) {
            ray.emplace_back(x, rounded(toZ, x, toX));
        }
    } else {
        for (std::int32_t z = 1; z <= toZ; ++z) {
            ray.emplace_back(rounded(toX, z, toZ), z);
        }
    }
}

/// Indexes an offset in [-radius, radius]^2 into a flat (2r+1)^2 scratch buffer.
[[nodiscard]] std::size_t offsetIndex(std::int32_t dx, std::int32_t dz,
                                      std::int32_t radius) noexcept {
    return static_cast<std::size_t>(dz + radius) * static_cast<std::size_t>(2 * radius + 1)
         + static_cast<std::size_t>(dx + radius);
}

/// One step of a ray: strikes the square off `visible` if the ground has risen in front
/// of it. Recoil's `CastLos` (`LosMap.cpp:525`), which is the whole occlusion rule.
void castStep(Fx& prevAngle, Fx& maxAngle, std::int32_t dx, std::int32_t dz,
              std::int32_t radius, std::span<const Fx> angles, std::span<char> visible) {
    const std::size_t index = offsetIndex(dx, dz, radius);
    const Fx angle = angles[index];

    // Below the horizon this ray has already climbed to: hidden.
    if (angle < maxAngle) {
        visible[index] = 0;
        return;
    }

    // The ray has just started to descend, so the square behind us was a crest. The
    // horizon rises to it, minus a slack that falls off with distance — near the emitter
    // the bonus is generous, far away it is nothing.
    if (angle < prevAngle) {
        const Fx distance = fxSqrt(Fx::fromInt(dx * dx + dz * dz));
        maxAngle = prevAngle - Fx::fromInt(kSightBonusElmos) / distance;
        if (angle < maxAngle) {
            visible[index] = 0;
            return;
        }
    }

    prevAngle = angle;
}

} // namespace

void raycastSquares(const IntelGrid& grid, const Terrain& terrain, Fx x, Fx z, Fx radius,
                    Fx eyeHeight, std::vector<std::int32_t>& squares) {
    squares.clear();

    const std::int32_t centre = grid.squareAt(x, z);
    if (centre == IntelGrid::kNoSquare) {
        return;
    }

    const std::int32_t squareElmos = grid.squareElmos().floorToInt();
    const std::int32_t radiusSquares = std::max(0, radius.floorToInt() / squareElmos);
    if (radiusSquares == 0) {
        squares.push_back(centre);
        return;
    }

    const std::int32_t squaresX = grid.squaresX();
    const std::int32_t squaresZ = grid.squaresZ();
    const std::int32_t centreX = centre % squaresX;
    const std::int32_t centreZ = centre / squaresX;
    const std::size_t span = static_cast<std::size_t>(2 * radiusSquares + 1);

    // START FROM THE DISC. Every square inside the radius is visible until a ray says
    // otherwise, which is exactly how `UnsafeLosAdd` sets `losRaySquares` before casting.
    std::vector<char> visible(span * span, char{0});
    std::vector<Fx> angles(span * span, kNoAngle);

    midpointCircleRows(radiusSquares, [&](std::int32_t halfWidth, std::int32_t rowZ) {
        for (std::int32_t dx = -halfWidth; dx <= halfWidth; ++dx) {
            const std::size_t index = offsetIndex(dx, rowZ, radiusSquares);
            visible[index] = 1;

            if (dx == 0 && rowZ == 0) {
                continue;
            }

            const std::int32_t column = centreX + dx;
            const std::int32_t row = centreZ + rowZ;
            if (column < 0 || column >= squaresX || row < 0 || row >= squaresZ) {
                visible[index] = 0;
                continue;
            }

            // The square's centre in world elmos, which is where its height is taken.
            const Fx worldX = Fx::fromInt(column * squareElmos + squareElmos / 2);
            const Fx worldZ = Fx::fromInt(row * squareElmos + squareElmos / 2);

            // GROUND BELOW SEA LEVEL COUNTS AS SEA LEVEL — `std::max(0.0f, ...)` at
            // `LosMap.cpp:653`. Otherwise a trench in front of a unit would raise the
            // horizon behind it, and sight would be blocked by a hole.
            const Fx ground = std::max(kFxZero, terrain.heightAt(worldX, worldZ));
            const Fx rise = ground - eyeHeight + Fx::fromInt(kSightBonusElmos);
            const Fx distance = fxSqrt(Fx::fromInt(dx * dx + rowZ * rowZ));
            angles[index] = rise / distance;
        }
    });

    // The rays. One to every square on the rim, plus one to any square inside the disc no
    // rim ray happened to pass through — zero-width lines miss squares, and Recoil's
    // `AddMissing` (`LosMap.cpp:232`) exists for exactly this. Ours is the same idea
    // arrived at by scanning rather than by their reverse walk from the 45-degree
    // bisector: simpler to read, and this runs per emitter rather than once per radius.
    std::vector<char> touched(span * span, char{0});
    std::vector<Ray> rays;
    Ray ray;

    const auto castRay = [&](std::int32_t toX, std::int32_t toZ) {
        buildRay(toX, toZ, ray);
        for (const auto& [dx, dz] : ray) {
            touched[offsetIndex(dx, dz, radiusSquares)] = 1;
        }
        rays.push_back(ray);
    };

    midpointCircleRows(radiusSquares, [&](std::int32_t halfWidth, std::int32_t rowZ) {
        if (rowZ < 0) {
            return;  // the quarter turn only; the other three are rotations of it
        }
        castRay(halfWidth, rowZ);
    });

    for (std::int32_t dz = 0; dz <= radiusSquares; ++dz) {
        for (std::int32_t dx = 0; dx <= radiusSquares; ++dx) {
            const std::size_t index = offsetIndex(dx, dz, radiusSquares);
            if (visible[index] != 0 && touched[index] == 0 && !(dx == 0 && dz == 0)) {
                castRay(dx, dz);
            }
        }
    }

    // Four rotations of the quarter, each with its own horizon — Recoil casts the same
    // four (`LosMap.cpp:674-677`).
    for (const Ray& line : rays) {
        Fx prevAngles[4] = {kNoAngle, kNoAngle, kNoAngle, kNoAngle};
        Fx maxAngles[4] = {kNoAngle, kNoAngle, kNoAngle, kNoAngle};

        for (const auto& [dx, dz] : line) {
            castStep(prevAngles[0], maxAngles[0], dx, dz, radiusSquares, angles, visible);
            castStep(prevAngles[1], maxAngles[1], -dx, -dz, radiusSquares, angles, visible);
            castStep(prevAngles[2], maxAngles[2], dz, -dx, radiusSquares, angles, visible);
            castStep(prevAngles[3], maxAngles[3], -dz, dx, radiusSquares, angles, visible);
        }
    }

    // Row-major, so the shape a caller holds is in one canonical order whatever the rays
    // did — which is what lets a test compare it against the disc directly.
    for (std::int32_t dz = -radiusSquares; dz <= radiusSquares; ++dz) {
        const std::int32_t row = centreZ + dz;
        if (row < 0 || row >= squaresZ) {
            continue;
        }
        for (std::int32_t dx = -radiusSquares; dx <= radiusSquares; ++dx) {
            const std::int32_t column = centreX + dx;
            if (column < 0 || column >= squaresX) {
                continue;
            }
            if (visible[offsetIndex(dx, dz, radiusSquares)] != 0) {
                squares.push_back(row * squaresX + column);
            }
        }
    }
}

void intelSquares(const IntelGrid& grid, const Terrain* terrain, VisionStyle style,
                  IntelKind kind, Fx x, Fx z, Fx radius, Fx eyeHeight,
                  std::vector<std::int32_t>& squares) {
    const bool raycast = style == VisionStyle::Recoil && terrain != nullptr
                      && (kind == IntelKind::Vision || kind == IntelKind::Radar);

    if (raycast) {
        raycastSquares(grid, *terrain, x, z, radius, eyeHeight, squares);
    } else {
        circleSquares(grid, x, z, radius, squares);
    }
}

void circleSquares(const IntelGrid& grid, Fx x, Fx z, Fx radius,
                   std::vector<std::int32_t>& squares) {
    squares.clear();

    const std::int32_t centre = grid.squareAt(x, z);
    if (centre == IntelGrid::kNoSquare) {
        return;
    }

    const std::int32_t squaresX = grid.squaresX();
    const std::int32_t squaresZ = grid.squaresZ();
    const std::int32_t centreX = centre % squaresX;
    const std::int32_t centreZ = centre / squaresX;

    // Elmos to squares, truncating — Recoil's `(unit->losRadius / SQUARE_SIZE) >> mipLevel`
    // (`LosHandler.cpp:139`). A radius that rounds to nothing still yields the one square
    // the emitter is standing on, which is what the loop below does at radius zero.
    const std::int32_t radiusSquares =
        std::max(0, radius.floorToInt() / grid.squareElmos().floorToInt());

    midpointCircleRows(radiusSquares, [&](std::int32_t halfWidth, std::int32_t rowZ) {
        const std::int32_t row = centreZ + rowZ;
        if (row < 0 || row >= squaresZ) {
            return;
        }
        // CLAMPED PER ROW, which is what stops a disc at the western border from
        // continuing into the eastern end of the row above it. The indices are one flat
        // array and nothing else would notice.
        const std::int32_t from = std::max(0, centreX - halfWidth);
        const std::int32_t to = std::min(squaresX - 1, centreX + halfWidth);
        for (std::int32_t column = from; column <= to; ++column) {
            squares.push_back(row * squaresX + column);
        }
    });
}



// --- The pass ---------------------------------------------------------------------

void Intel::configure(std::size_t alliances, Fx widthElmos, Fx depthElmos,
                      VisionStyle style) {
    style_ = style;
    grids_.clear();
    placements_.clear();
    emitters_.clear();

    grids_.reserve(alliances * kIntelKindCount);
    for (std::size_t alliance = 0; alliance < alliances; ++alliance) {
        grids_.emplace_back(widthElmos, depthElmos, kVisionMipLevel);
        grids_.emplace_back(widthElmos, depthElmos, kRadarMipLevel);
        grids_.emplace_back(widthElmos, depthElmos, kRadarMipLevel);
    }
}

const IntelGrid& Intel::grid(int alliance, IntelKind kind) const noexcept {
    static const IntelGrid kEmpty{};
    const auto index = static_cast<std::size_t>(alliance) * kIntelKindCount
                     + static_cast<std::size_t>(kind);
    return index < grids_.size() ? grids_[index] : kEmpty;
}

bool Intel::sees(int alliance, IntelKind kind, Fx x, Fx z) const noexcept {
    if (!active()) {
        return true;
    }
    if (alliance < 0 || static_cast<std::size_t>(alliance) >= alliances()) {
        return false;
    }
    return grid(alliance, kind).covered(x, z);
}

void Intel::withdraw(UnitIndex slot) {
    Placement& placement = placements_[slot];
    if (placement.square == IntelGrid::kNoSquare) {
        return;
    }

    for (std::size_t kind = 0; kind < kIntelKindCount; ++kind) {
        std::vector<std::int32_t>& squares = emitters_[slot][kind].squares;
        if (squares.empty()) {
            continue;
        }
        const auto index = static_cast<std::size_t>(placement.alliance) * kIntelKindCount + kind;
        grids_[index].remove(squares);
        squares.clear();
    }
    placement.square = IntelGrid::kNoSquare;
}

void Intel::update(const UnitStore& store, const UnitCatalog& catalog,
                   std::span<const Army> armies, const Terrain* terrain) {
    if (!active()) {
        return;
    }

    const std::size_t slots = store.slotCount();
    placements_.resize(slots);
    emitters_.resize(slots);

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < slots; ++slot) {
        // A dead unit stops seeing. Recoil holds an instance for 1.5 seconds after death so
        // sight does not blink off on an explosion (`DelayedFreeInstance`); we withdraw at
        // once, because that delay only exists to make its instance CACHE worth having and
        // we have no cache. The visible difference is a wreck's last square of ground going
        // dark a second earlier.
        if (!store.slotAlive(slot)) {
            withdraw(slot);
            continue;
        }

        const int army = motion[slot].armyIndex;
        if (army == kNoArmy || static_cast<std::size_t>(army) >= armies.size()) {
            withdraw(slot);
            continue;
        }

        const int alliance = armies[static_cast<std::size_t>(army)].alliance;
        const Transform& at = transforms[slot];

        // The SQUARE decides, not the position. A unit crossing a 16-elmo square at 27
        // elmos a second re-stamps about twice a second; stamping on every position change
        // would do the same work ten times over for an answer the grid cannot express.
        //
        // Sight's grid is the finest of the three, so its square is the one that governs —
        // a move too small to change it cannot change radar's coarser one either.
        const std::int32_t square = grid(alliance, IntelKind::Vision).squareAt(at.x, at.z);
        Placement& placement = placements_[slot];
        if (placement.square == square && placement.alliance == alliance
            && square != IntelGrid::kNoSquare) {
            continue;
        }

        withdraw(slot);
        if (square == IntelGrid::kNoSquare) {
            continue;  // off the map: seeing nothing is right, and so is re-checking next tick
        }

        const UnitCatalog::IntelRadii& radii = catalog.intel(store.typeAt(slot));
        const Fx byKind[kIntelKindCount] = {radii.vision, radii.radar, radii.sonar};

        for (std::size_t kind = 0; kind < kIntelKindCount; ++kind) {
            if (byKind[kind] <= kFxZero) {
                continue;
            }
            const auto index =
                static_cast<std::size_t>(alliance) * kIntelKindCount + kind;
            intelSquares(grids_[index], terrain, style_, static_cast<IntelKind>(kind), at.x,
                         at.z, byKind[kind], at.y, scratch_);

            std::vector<std::int32_t>& squares = emitters_[slot][kind].squares;
            squares.assign(scratch_.begin(), scratch_.end());
            grids_[index].add(squares);
        }

        placement.square = square;
        placement.alliance = alliance;
    }
}

} // namespace rm::sim
