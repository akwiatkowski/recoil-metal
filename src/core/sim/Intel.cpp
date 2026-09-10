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
    hiddenGrids_.clear();
    placements_.clear();
    emitters_.clear();
    hiddenEmitters_.clear();
    retainedRadarContacts_.clear();
    retainedRadarContacts_.resize(alliances);
    seenEver_.clear();
    seenEver_.resize(alliances);

    // ONE GRID PER KIND PER ALLIANCE, IN `IntelKind` ORDER, because every index into this is
    // `alliance * kIntelKindCount + kind` and nothing bounds-checks it. Adding a kind without
    // adding a grid here reads and writes past the end of an alliance's block into the next
    // one's — which is exactly what happened when Omni arrived, and it showed up as an
    // alliance losing its VISION rather than as anything to do with omni.
    grids_.reserve(alliances * kIntelKindCount);
    for (std::size_t alliance = 0; alliance < alliances; ++alliance) {
        grids_.emplace_back(widthElmos, depthElmos, kVisionMipLevel);   // Vision
        grids_.emplace_back(widthElmos, depthElmos, kRadarMipLevel);    // Radar
        grids_.emplace_back(widthElmos, depthElmos, kRadarMipLevel);    // Sonar
        // OMNI AT RADAR'S MIP. This used to be the vision mip, argued from precision: omni
        // radii are mostly small and an omni return carries an IDENTITY, so it looked like it
        // wanted sight's resolution rather than a blip's. Retail disagrees — omni is one of
        // its scale-4 grids, alongside radar, sonar, water vision and the counter-intel
        // family, with only fog at scale 2 (`C-077`).
        //
        // The argument was not wrong about what omni *is*; it was reasoning about a choice
        // the original had already made. Because radius-to-cells truncates, the finer grid put
        // our omni edge up to two world units outside retail's for any radius that is not a
        // multiple of four ogrids.
        grids_.emplace_back(widthElmos, depthElmos, kRadarMipLevel);    // Omni
    }
    assert(grids_.size() == alliances * kIntelKindCount);

    // The "hidden here" family, one per HiddenKind per alliance and in that order — the
    // same rule and the same reason as above. Radar's mip for both: a stealth field is a
    // radar-scaled thing, and its edge is as invisible as radar's own.
    hiddenGrids_.reserve(alliances * kHiddenKindCount);
    for (std::size_t alliance = 0; alliance < alliances; ++alliance) {
        hiddenGrids_.emplace_back(widthElmos, depthElmos, kRadarMipLevel);  // RadarField
        hiddenGrids_.emplace_back(widthElmos, depthElmos, kRadarMipLevel);  // SonarField
    }
    assert(hiddenGrids_.size() == alliances * kHiddenKindCount);
}

const IntelGrid& Intel::grid(int alliance, IntelKind kind) const noexcept {
    static const IntelGrid kEmpty{};
    const auto index = static_cast<std::size_t>(alliance) * kIntelKindCount
                     + static_cast<std::size_t>(kind);
    return index < grids_.size() ? grids_[index] : kEmpty;
}

const IntelGrid& Intel::hiddenGrid(int alliance, HiddenKind kind) const noexcept {
    static const IntelGrid kEmpty{};
    const auto index = static_cast<std::size_t>(alliance) * kHiddenKindCount
                     + static_cast<std::size_t>(kind);
    return index < hiddenGrids_.size() ? hiddenGrids_[index] : kEmpty;
}

bool Intel::hiddenBy(int ownerAlliance, HiddenKind kind, Fx x, Fx z) const noexcept {
    if (!active()) {
        return false;  // no intel means no fog, and no fog means nothing to hide in
    }
    if (ownerAlliance < 0 || static_cast<std::size_t>(ownerAlliance) >= alliances()) {
        return false;
    }
    return hiddenGrid(ownerAlliance, kind).covered(x, z);
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

std::span<const RetainedRadarContact> Intel::retainedRadarContacts(int alliance) const noexcept {
    static const std::vector<RetainedRadarContact> kEmpty;
    if (alliance < 0 || static_cast<std::size_t>(alliance) >= retainedRadarContacts_.size()) {
        return kEmpty;
    }
    return retainedRadarContacts_[static_cast<std::size_t>(alliance)];
}

bool Intel::hasSeenEver(int alliance, UnitId unit) const noexcept {
    const std::span<const UnitId> known = seenEver(alliance);
    return unit.index < known.size() && known[unit.index] == unit;
}

std::span<const UnitId> Intel::seenEver(int alliance) const noexcept {
    static const std::vector<UnitId> kEmpty;
    if (alliance < 0 || static_cast<std::size_t>(alliance) >= seenEver_.size()) {
        return kEmpty;
    }
    return seenEver_[static_cast<std::size_t>(alliance)];
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
    for (std::size_t kind = 0; kind < kHiddenKindCount; ++kind) {
        std::vector<std::int32_t>& squares = hiddenEmitters_[slot][kind].squares;
        if (squares.empty()) {
            continue;
        }
        const auto index =
            static_cast<std::size_t>(placement.alliance) * kHiddenKindCount + kind;
        hiddenGrids_[index].remove(squares);
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
    hiddenEmitters_.resize(slots);

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
        const Fx byKind[kIntelKindCount] = {radii.vision, radii.radar, radii.sonar,
                                            radii.omni};

        for (std::size_t kind = 0; kind < kIntelKindCount; ++kind) {
            if (byKind[kind] <= kFxZero) {
                continue;
            }
            const auto index =
                static_cast<std::size_t>(alliance) * kIntelKindCount + kind;
            // THE EYE, not the feet. `at.y` is the ground the unit stands on; the sensor is on
            // top of it, and handing the raycast the transform meant a commander looked out
            // from ankle level and could not see over a rise its own head cleared.
            // `VisionStyle::ForgedAlliance` ignores the height entirely, as its own shader
            // does.
            intelSquares(grids_[index], terrain, style_, static_cast<IntelKind>(kind), at.x,
                         at.z, byKind[kind], at.y + radii.eyeHeight, scratch_);

            std::vector<std::int32_t>& squares = emitters_[slot][kind].squares;
            squares.assign(scratch_.begin(), scratch_.end());
            grids_[index].add(squares);
        }

        // The stealth FIELDS this unit projects, into the hidden family. Discs always: the
        // fields are a Forged Alliance mechanic and FA stamps discs for everything — a
        // raycast stealth shadow would be an invention, not a transcription.
        const Fx byHidden[kHiddenKindCount] = {radii.radarStealthField,
                                               radii.sonarStealthField};
        for (std::size_t kind = 0; kind < kHiddenKindCount; ++kind) {
            if (byHidden[kind] <= kFxZero) {
                continue;
            }
            const auto index =
                static_cast<std::size_t>(alliance) * kHiddenKindCount + kind;
            circleSquares(hiddenGrids_[index], at.x, at.z, byHidden[kind], scratch_);

            std::vector<std::int32_t>& squares = hiddenEmitters_[slot][kind].squares;
            squares.assign(scratch_.begin(), scratch_.end());
            hiddenGrids_[index].add(squares);
        }

        placement.square = square;
        placement.alliance = alliance;
    }

    // C-158's `RECON_LOSEver` is per viewing alliance and full unit identity, not a property of
    // the current contact. Update it only after every emitter has stamped this tick, otherwise a
    // later vision source could lose to an earlier radar source by iteration order.
    for (int alliance = 0; alliance < static_cast<int>(alliances()); ++alliance) {
        std::vector<UnitId>& known = seenEver_[static_cast<std::size_t>(alliance)];
        known.resize(slots);
        for (UnitIndex slot = 0; slot < slots; ++slot) {
            if (!store.slotAlive(slot)) {
                known[slot] = {};
                continue;
            }
            const UnitId unit = store.idAt(slot);
            if (known[slot] != unit) {
                known[slot] = {};
            }
            if (contactKindForUnit(alliance, slot, store, catalog, armies, *this)
                == ContactKind::Seen) {
                known[slot] = unit;
            }
        }
    }

    // A radar return is knowledge owned by its VIEWER, not by the observed unit. Refresh the
    // last known position while radar sees a live source; leave it behind if that source dies.
    // A later generation in the same slot is concrete contrary evidence: retaining both would
    // project one stale blip alongside the replacement forever. This is deliberately not a
    // time-based expiry or a general re-acquisition policy.
    for (std::vector<RetainedRadarContact>& contacts : retainedRadarContacts_) {
        std::erase_if(contacts, [&store, slots](const RetainedRadarContact& contact) {
            return contact.unit.index < slots && store.slotAlive(contact.unit.index)
                   && store.idAt(contact.unit.index) != contact.unit;
        });
        for (RetainedRadarContact& contact : contacts) {
            const bool sourceAlive = contact.unit.index < slots
                                  && store.slotAlive(contact.unit.index)
                                  && store.idAt(contact.unit.index).generation
                                         == contact.unit.generation;
            contact.maybeDead = !sourceAlive;
        }
    }
    for (int alliance = 0; alliance < static_cast<int>(alliances()); ++alliance) {
        std::vector<RetainedRadarContact>& contacts =
            retainedRadarContacts_[static_cast<std::size_t>(alliance)];
        for (UnitIndex slot = 0; slot < slots; ++slot) {
            if (!store.slotAlive(slot)) {
                continue;
            }
            if (contactKindForUnit(alliance, slot, store, catalog, armies, *this)
                != ContactKind::Radar) {
                continue;
            }

            const UnitId unit = store.idAt(slot);
            const Transform& at = transforms[slot];
            const auto found = std::ranges::find_if(
                contacts, [unit](const RetainedRadarContact& contact) {
                    return contact.unit.index == unit.index
                        && contact.unit.generation == unit.generation;
                });
            if (found == contacts.end()) {
                contacts.push_back({.unit = unit, .x = at.x, .z = at.z});
            } else {
                found->x = at.x;
                found->z = at.z;
                found->maybeDead = false;
            }
        }
    }
}


// --- Contacts ---------------------------------------------------------------------

namespace {

/// A stable 32-bit mix of three numbers. Not cryptographic and not meant to be — what is
/// needed is that the same inputs give the same answer everywhere, which integer
/// multiplication and shifting do and a `<random>` engine's implementation-defined
/// internals do not.
[[nodiscard]] std::uint32_t mix(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
    std::uint32_t h = 2166136261u;
    for (const std::uint32_t value : {a, b, c}) {
        h ^= value;
        h *= 16777619u;
        h ^= h >> 15;
    }
    return h;
}

/// The direction a blip's error points in during `bucket`, as an angle.
[[nodiscard]] Brad blipAngle(UnitId unit, std::uint32_t bucket) noexcept {
    return static_cast<Brad>(mix(unit.index, unit.generation, bucket) & 0xFFFFu);
}

} // namespace

std::array<Fx, 2> radarBlipPosition(UnitId unit, Fx x, Fx z, TickIndex tick,
                                    TickRate rate) noexcept {
    // The period in ticks, derived from the rate rather than written down — §5.1. `ticks`
    // floors at one, so an absurd rate degrades to a fresh direction every tick rather than
    // to a division by zero.
    const auto period = static_cast<TickIndex>(rate.ticks(kBlipDriftPeriod));
    const auto bucket = static_cast<std::uint32_t>(tick / period);
    const auto within = static_cast<std::int32_t>(tick % period);

    // Between this bucket's direction and the next, so the blip WANDERS rather than
    // teleporting every fifteenth tick — which is what Recoil's 1/256-per-frame slide
    // achieves and what makes a radar contact read as an uncertain position rather than a
    // flickering one.
    const Brad from = blipAngle(unit, bucket);
    const Brad to = blipAngle(unit, bucket + 1);
    const Fx blend = Fx::fromInt(within) / Fx::fromInt(static_cast<std::int32_t>(period));

    const Fx radius = Fx::fromInt(kRadarErrorElmos);
    const Fx fromX = fxCos(from) * radius;
    const Fx fromZ = fxSin(from) * radius;
    const Fx toX = fxCos(to) * radius;
    const Fx toZ = fxSin(to) * radius;

    return {x + fromX + (toX - fromX) * blend, z + fromZ + (toZ - fromZ) * blend};
}

std::optional<ContactKind> contactKindForUnit(int alliance, UnitIndex target,
                                              const UnitStore& store,
                                              const UnitCatalog& catalog,
                                              std::span<const Army> armies,
                                              const Intel& intel) noexcept {
    if (!store.slotAlive(target)) {
        return std::nullopt;
    }
    const int armyIndex = store.motion()[target].armyIndex;
    const auto army = std::ranges::find_if(
        armies, [armyIndex](const Army& candidate) { return candidate.index == armyIndex; });
    if (army == armies.end()) {
        return std::nullopt;
    }

    const Transform& at = store.transforms()[target];
    if (army->alliance == alliance || !intel.active()) {
        return ContactKind::Seen;
    }

    const UnitCatalog::IntelRadii& hiding = catalog.intel(store.typeAt(target));
    // Depth gates the senses, never the geometry: a submerged submarine is invisible
    // to vision and radar however close it stands, and sonar only ever hears naval
    // hulls — surface ships and submarines, surfaced or not. Omni still sees all.
    const MoveState& targetMotion = store.motion()[target];
    const bool submerged = targetMotion.submersible && targetMotion.submerged;
    const bool naval = targetMotion.surfaceWater || targetMotion.submersible;
    if (hiding.freeIntel || intel.sees(alliance, IntelKind::Omni, at.x, at.z)
        || (!hiding.cloak && !submerged && intel.sees(alliance, IntelKind::Vision, at.x, at.z))) {
        return ContactKind::Seen;
    }
    // Cloak defeats every non-omni sense: a cloaked unit under a T1 dish is absent,
    // not a blip, and only omni (the T3 sensor sense) brings it back. RadarStealth
    // and SonarStealth each defeat their own sense the same way.
    if (!submerged && !hiding.cloak && !hiding.radarStealth
        && !intel.hiddenBy(army->alliance, HiddenKind::RadarField, at.x, at.z)
        && intel.sees(alliance, IntelKind::Radar, at.x, at.z)) {
        return ContactKind::Radar;
    }
    if (naval && !hiding.cloak && !hiding.sonarStealth
        && !intel.hiddenBy(army->alliance, HiddenKind::SonarField, at.x, at.z)
        && intel.sees(alliance, IntelKind::Sonar, at.x, at.z)) {
        return ContactKind::Sonar;
    }
    return std::nullopt;
}

void contactsFor(int alliance, const UnitStore& store, const UnitCatalog& catalog,
                 std::span<const Army> armies,
                 const Intel& intel, TickIndex tick, std::vector<Contact>& contacts,
                 TickRate rate) {
    contacts.clear();

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        const int army = motion[slot].armyIndex;
        if (army == kNoArmy || static_cast<std::size_t>(army) >= armies.size()) {
            continue;  // unowned scenery is nobody's contact
        }

        const Transform& at = transforms[slot];
        const UnitCatalog::IntelRadii& hiding = catalog.intel(store.typeAt(slot));
        const std::optional<ContactKind> kind =
            contactKindForUnit(alliance, slot, store, catalog, armies, intel);
        if (kind == ContactKind::Seen) {
            contacts.push_back(Contact{.unit = store.idAt(slot),
                                       .x = at.x,
                                       .z = at.z,
                                       .kind = ContactKind::Seen});
            continue;
        }
        if (kind) {
            const auto [x, z] = radarBlipPosition(store.idAt(slot), at.x, at.z, tick, rate);
            contacts.push_back(Contact{.unit = store.idAt(slot),
                                       .x = x,
                                       .z = z,
                                       .kind = *kind});
        }

        // THE JAMMER: `jammerBlips` false radar contacts scattered inside `jamRadius` of a
        // hostile carrier, whenever the viewer's radar covers the carrier's ground — a
        // deception needs a sense to deceive. Emitted WHETHER OR NOT the carrier itself
        // resolved to a contact above: a radar-stealthed jammer that showed nothing but its
        // own blips would be the strongest version of the trick, and that is the shipped
        // XES0102's exact loadout. Each blip wanders on the same derived drift the real
        // ones use, seeded by the blip's ordinal, so the cluster reads as contacts rather
        // than as a ring of satellites.
        if (hiding.jammerBlips > 0 && hiding.jamRadius > kFxZero
            && intel.sees(alliance, IntelKind::Radar, at.x, at.z)) {
            const UnitId carrier = store.idAt(slot);
            for (int blip = 0; blip < hiding.jammerBlips; ++blip) {
                // A distinct identity per blip, derived from the carrier's: the generation
                // offset keeps the angle hash from handing every blip the same wander.
                const UnitId ghost{carrier.index,
                                   static_cast<Generation>(
                                       carrier.generation
                                       + static_cast<Generation>(blip + 1))};
                const Brad spread = blipAngle(ghost, 0x9E37u + static_cast<std::uint32_t>(blip));
                const Fx offsetX = fxCos(spread) * hiding.jamRadius;
                const Fx offsetZ = fxSin(spread) * hiding.jamRadius;
                const auto [x, z] =
                    radarBlipPosition(ghost, at.x + offsetX, at.z + offsetZ, tick, rate);
                contacts.push_back(Contact{.unit = carrier,
                                           .x = x,
                                           .z = z,
                                           .kind = ContactKind::Radar});
            }
        }
    }

    // A dead source cannot contribute to the live-slot projection above, but the alliance's
    // retained radar knowledge still projects as a blip at its last confirmed position.
    for (const RetainedRadarContact& retained : intel.retainedRadarContacts(alliance)) {
        // A slot can be recycled after `Intel::update` and before this projection. The new
        // generation is decisive contrary evidence even before the next retained-contact pass.
        if (retained.unit.index < store.slotCount() && store.slotAlive(retained.unit.index)
            && store.idAt(retained.unit.index) != retained.unit) {
            continue;
        }
        // `Intel::update` precedes retirement in `tickSkirmish`. A source can therefore die
        // after its cache refresh but before this projection in the same tick; consult the
        // generational handle as well as the persisted marker so that tick does not lose it.
        if (!retained.maybeDead && store.alive(retained.unit)) {
            continue;
        }
        const auto [x, z] =
            radarBlipPosition(retained.unit, retained.x, retained.z, tick, rate);
        contacts.push_back(Contact{.unit = retained.unit,
                                    .x = x,
                                    .z = z,
                                    .kind = ContactKind::Radar,
                                    .maybeDead = retained.maybeDead});
    }
}

} // namespace rm::sim
