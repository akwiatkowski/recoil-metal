#include "core/sim/Terrain.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace rm::sim {

Fx snapToBuildGrid(Fx centre, int footprintSquares) noexcept {
    // Even footprints centre on a grid line, odd ones on a cell centre: a 2-wide structure
    // spans two whole cells, a 1-wide one sits inside one. Rounding is to the nearest such
    // point, computed on the raw fixed-point words so negative coordinates floor correctly.
    const std::int64_t pitch = kBuildGridElmos.raw();
    const std::int64_t half = (std::max(footprintSquares, 1) % 2 == 1) ? pitch / 2 : 0;
    const std::int64_t shifted = static_cast<std::int64_t>(centre.raw()) - half + pitch / 2;
    std::int64_t cells = shifted / pitch;
    if (shifted < 0 && shifted % pitch != 0) --cells;  // floor, not truncation
    return Fx::fromRaw(static_cast<std::int32_t>(cells * pitch + half));
}

std::array<Fx, 2> Terrain::buildSite(const unitdef::UnitDef& def, Fx x, Fx z) const noexcept {
    if (placement_ == PlacementMode::Free
        || def.buildRestriction != unitdef::BuildRestriction::None) {
        return {x, z};  // free mode, or a deposit whose own centre is the site
    }
    return {snapToBuildGrid(x, def.footprintSquaresX), snapToBuildGrid(z, def.footprintSquaresZ)};
}

Terrain::Terrain(const HeightField& field, bool hasWater, float waterLevelElmos,
                 const MaxHeightPyramid* lookAhead,
                 std::span<const ResourceDeposit> deposits, PlacementMode placement) noexcept
    : deposits_(deposits), placement_(placement), field_(&field),
      lookAhead_(lookAhead),
      baseHeight_(fxFromFloat(field.baseHeight)),
      hasWater_(hasWater),
      waterLevel_(fxFromFloat(waterLevelElmos)),
      // Construction is content load, so the float is allowed here — it is the boundary, and
      // it is crossed once per map rather than once per sample.
      heightScale_(static_cast<FxWide>(
          std::llround(static_cast<double>(field.heightScale)
                       * static_cast<double>(FxWide{1} << kScaleBits)))) {}

bool Terrain::resourceSitePlaceable(unitdef::BuildRestriction restriction, Fx x, Fx z) const noexcept {
    if (restriction == unitdef::BuildRestriction::None) return true;
    // Orders use the marker's exact fixed-point centre; the UI snaps before issuing them.
    return std::ranges::any_of(deposits_, [&](const ResourceDeposit& deposit) {
        return deposit.kind == restriction && deposit.x == x && deposit.z == z;
    });
}

Fx Terrain::cornerHeight(std::int32_t x, std::int32_t z) const noexcept {
    const std::int32_t cx = std::clamp(x, 0, field_->squaresX);
    const std::int32_t cz = std::clamp(z, 0, field_->squaresZ);

    const auto index = static_cast<std::size_t>(cz)
                           * static_cast<std::size_t>(field_->verticesX())
                       + static_cast<std::size_t>(cx);
    if (index >= field_->raw.size()) {
        return baseHeight_;
    }

    return decodeRaw(field_->raw[index]);
}

Fx Terrain::decodeRaw(std::uint16_t raw) const noexcept {
    // `raw` is a `uint16` and the scale is kept to 2^-30, so the product is exact to well
    // inside one step of the result. 65,535 times a scale of ~0.01 at 2^30 is about 7e11 —
    // comfortable in 64 bits, and nowhere near what a 32-bit intermediate would survive.
    const FxWide scaled = FxWide{raw} * heightScale_;
    return baseHeight_ + Fx::fromRaw(saturate(roundShift(scaled, kScaleBits
                                                                     - kFxFractionalBits)));
}

Fx Terrain::heightAt(Fx x, Fx z) const noexcept {
    if (field_->squaresX <= 0 || field_->squaresZ <= 0) {
        return baseHeight_;
    }

    // World elmos to grid coordinates. A square is 8 elmos and 8 is a power of two, so this
    // division is exact — no rounding enters the sample position at all, which is a small
    // piece of luck worth noting: on a grid of any other pitch the fraction below would carry
    // a rounding error into every height.
    const Fx gridX = x / Fx::fromInt(kSquareSize);
    const Fx gridZ = z / Fx::fromInt(kSquareSize);

    // Clamped BEFORE the floor, so a position far off the map cannot overflow the cast — the
    // same order `heightAtWorld` uses and for the same reason.
    const Fx clampedX = std::clamp(gridX, Fx{}, Fx::fromInt(field_->squaresX));
    const Fx clampedZ = std::clamp(gridZ, Fx{}, Fx::fromInt(field_->squaresZ));

    // `floorToInt`, not a truncation: truncation rounds toward zero and would mirror the
    // interpolation for negative coordinates instead of clamping it.
    const std::int32_t x0 = clampedX.floorToInt();
    const std::int32_t z0 = clampedZ.floorToInt();
    const Fx fx = clampedX - Fx::fromInt(x0);
    const Fx fz = clampedZ - Fx::fromInt(z0);

    const Fx h00 = cornerHeight(x0, z0);
    const Fx h10 = cornerHeight(x0 + 1, z0);
    const Fx h01 = cornerHeight(x0, z0 + 1);
    const Fx h11 = cornerHeight(x0 + 1, z0 + 1);

    // Interpolating decoded heights rather than raw words is equivalent — the decode is
    // affine — and keeps everything in one type.
    const Fx alongZ0 = h00 + (h10 - h00) * fx;
    const Fx alongZ1 = h01 + (h11 - h01) * fx;
    return alongZ0 + (alongZ1 - alongZ0) * fz;
}

Fx Terrain::surfaceHeightAt(Fx x, Fx z) const noexcept {
    const Fx ground = heightAt(x, z);
    return hasWater_ ? std::max(ground, waterLevel_) : ground;
}

Fx Terrain::deepHeightAt(Fx x, Fx z) const noexcept {
    // `C-019`'s `GetDeepElevation`: the ground clamped to the deep level —
    // terrain above the level answers the level, terrain below it answers
    // itself. Undeclared deep level falls back to the water level, retail's
    // own default.
    const Fx level = hasDeepLevel_ ? deepLevel_ : waterLevel_;
    return std::min(heightAt(x, z), level);
}

Fx Terrain::abyssHeightAt(Fx x, Fx z) const noexcept {
    // Same clamp at the abyss level (`GetAbyssElevation`).
    const Fx level = hasAbyssLevel_ ? abyssLevel_ : waterLevel_;
    return std::min(heightAt(x, z), level);
}

bool Terrain::isPlayable(Fx x, Fx z) const noexcept {
    if (!hasPlayableRect_) {
        return true;  // a map with no declared rect is playable everywhere
    }
    return x >= playableX0_ && x <= playableX1_ && z >= playableZ0_
           && z <= playableZ1_;
}

void Terrain::setPlayableRect(Fx x0, Fx z0, Fx x1, Fx z1) noexcept {
    playableX0_ = x0;
    playableZ0_ = z0;
    playableX1_ = x1;
    playableZ1_ = z1;
    hasPlayableRect_ = true;
}

void Terrain::setWaterLevels(float deepElmos, float abyssElmos) noexcept {
    deepLevel_ = fxFromFloat(deepElmos);
    abyssLevel_ = fxFromFloat(abyssElmos);
    hasDeepLevel_ = true;
    hasAbyssLevel_ = true;
}

void Terrain::flattenRect(Fx x0Elmos, Fx z0Elmos, Fx x1Elmos, Fx z1Elmos,
                          Fx elevation) noexcept {
    // Elmos to cells: `floor` on the near edge, `ceil` on the far — the same pair
    // `StructureUnit.FlattenSkirt` applies to `GetSkirtRect` (defaultunits.lua:70-71).
    // Raw-word arithmetic, NOT `Fx` division: `Fx::divide` rounds to nearest, which would
    // grow the far edge a cell whenever the remainder rounds up. `ceil(v/p)` as integer
    // `floor((v + p - 1)/p)` is exact, and floor division needs the negative-remainder
    // correction because C++ truncates toward zero.
    const auto floorDiv = [](std::int64_t n, std::int64_t d) noexcept -> std::int32_t {
        const std::int64_t q = n / d;
        return static_cast<std::int32_t>((n < 0 && n % d != 0) ? q - 1 : q);
    };
    const std::int64_t pitch = Fx::fromInt(kSquareSize).raw();
    const int x0 = floorDiv(x0Elmos.raw(), pitch);
    const int z0 = floorDiv(z0Elmos.raw(), pitch);
    const int x1 = floorDiv(x1Elmos.raw() + pitch - 1, pitch);
    const int z1 = floorDiv(z1Elmos.raw() + pitch - 1, pitch);

    // The field is plumbed const because mutation is the exception — this is the one
    // write the sim makes to the map it otherwise only reads (`Sim::FlattenMapRect`
    // mutates retail's `STIMap` heightfield the same way). Every field this view wraps
    // is mutable storage (`LoadedMap::field`, test locals), so the cast is the
    // sanctioned write, not a lie about a truly-const object.
    HeightField& field = const_cast<HeightField&>(*field_);
    field.setElevationRect(x0, z0, x1, z1, fxToFloat(elevation));
}

Fx Terrain::maxSurfaceHeightNear(Fx x, Fx z, Fx reachElmos) const noexcept {
    // Retail works in ogrids — one heightmap sample per ogrid, one square here — and
    // takes the point sample under a reach of one (`0x006340f0`).
    const Fx reachSquares = reachElmos / Fx::fromInt(kSquareSize);
    if (reachSquares < Fx::fromInt(1) || field_->squaresX <= 0 || field_->squaresZ <= 0) {
        return surfaceHeightAt(x, z);
    }

    // The pyramid level: the highest set bit of half the reach, plus one — so the cell is
    // at least half the reach wide and less than the whole of it — capped by the level
    // whose cell is the map. `bsr` of a positive integer is `bit_width − 1`.
    const auto bsr = [](std::int32_t value) noexcept -> int {
        return value > 0 ? std::bit_width(static_cast<std::uint32_t>(value)) - 1 : -1;
    };
    const int mapCap = bsr(std::min(field_->squaresX, field_->squaresZ) - 2) + 1;
    int level = bsr((reachSquares * Fx::fromRatio(1, 2)).floorToInt()) + 1;
    level = std::max(1, std::min(level, mapCap));

    const Fx gridX = std::clamp(x / Fx::fromInt(kSquareSize), Fx{}, Fx::fromInt(field_->squaresX));
    const Fx gridZ = std::clamp(z / Fx::fromInt(kSquareSize), Fx{}, Fx::fromInt(field_->squaresZ));
    const std::int32_t cellX = gridX.floorToInt() >> level;
    const std::int32_t cellZ = gridZ.floorToInt() >> level;

    // The pyramid answers in one load. It holds raw maxima, which are height maxima only
    // while the scale is positive; a downhill scale takes the scan below instead.
    if (lookAhead_ != nullptr && heightScale_ > 0 && level <= lookAhead_->levelCount()) {
        const Fx highest = decodeRaw(lookAhead_->maxRaw(level, cellX, cellZ));
        return hasWater_ ? std::max(highest, waterLevel_) : highest;
    }

    const std::int32_t first = 1 << level;

    // Every corner of the cell, inclusive of its far edge: a square's height is decided by
    // its four corners, so the last corner row belongs to the cell as much as the first.
    Fx highest = baseHeight_;
    bool any = false;
    for (std::int32_t cz = cellZ * first; cz <= (cellZ + 1) * first; ++cz) {
        for (std::int32_t cx = cellX * first; cx <= (cellX + 1) * first; ++cx) {
            const Fx h = cornerHeight(cx, cz);
            highest = any ? std::max(highest, h) : h;
            any = true;
        }
    }
    return hasWater_ ? std::max(highest, waterLevel_) : highest;
}

TerrainTypeGrid::TerrainTypeGrid(std::span<const std::uint8_t> base, int squaresX,
                                 int squaresZ)
    : squaresX_(std::max(0, squaresX)), squaresZ_(std::max(0, squaresZ)) {
    const std::size_t cells = static_cast<std::size_t>(squaresX_)
                              * static_cast<std::size_t>(squaresZ_);
    if (base.size() == cells) {
        base_.assign(base.begin(), base.end());
    } else {
        // A mismatched or absent grid reads as all-default — the same answer
        // `buildPassability` gives a span that does not match the field.
        base_.assign(cells, std::uint8_t{0});
    }
    effective_ = base_;
}

bool TerrainTypeGrid::cellRect(Fx x0Elmos, Fx z0Elmos, Fx x1Elmos, Fx z1Elmos,
                               TerrainStamp& out) const noexcept {
    // Elmos to cells: `floor` on the near edge, `ceil` on the far — the same
    // pair `StructureUnit.FlattenSkirt` applies to `GetSkirtRect`
    // (defaultunits.lua:70-71) and `flattenRect` applies above. Raw-word
    // arithmetic, not `Fx` division: `Fx::divide` rounds to nearest, which
    // would grow the far edge a cell whenever the remainder rounds up.
    const auto floorDiv = [](std::int64_t n, std::int64_t d) noexcept -> std::int32_t {
        const std::int64_t q = n / d;
        return static_cast<std::int32_t>((n < 0 && n % d != 0) ? q - 1 : q);
    };
    const std::int64_t pitch = Fx::fromInt(kSquareSize).raw();
    out.x0 = std::clamp(floorDiv(x0Elmos.raw(), pitch), 0, squaresX_);
    out.z0 = std::clamp(floorDiv(z0Elmos.raw(), pitch), 0, squaresZ_);
    out.x1 = std::clamp(floorDiv(x1Elmos.raw() + pitch - 1, pitch), 0, squaresX_);
    out.z1 = std::clamp(floorDiv(z1Elmos.raw() + pitch - 1, pitch), 0, squaresZ_);
    return out.x0 < out.x1 && out.z0 < out.z1;
}

void TerrainTypeGrid::rematerialize(int x0, int z0, int x1, int z1) noexcept {
    for (int z = z0; z < z1; ++z) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t cell = static_cast<std::size_t>(z)
                                         * static_cast<std::size_t>(squaresX_)
                                     + static_cast<std::size_t>(x);
            std::uint8_t type = base_[cell];
            // Last write wins: the newest journal entry covering the cell is
            // its type, so a lifted stamp reveals the write beneath it rather
            // than the map's base.
            for (const TerrainStamp& entry : journal_) {
                if (x >= entry.x0 && x < entry.x1 && z >= entry.z0 && z < entry.z1) {
                    type = entry.type;
                }
            }
            effective_[cell] = type;
        }
    }
}

void TerrainTypeGrid::setRect(Fx x0Elmos, Fx z0Elmos, Fx x1Elmos, Fx z1Elmos,
                              std::uint8_t type) {
    stamp(x0Elmos, z0Elmos, x1Elmos, z1Elmos, type, UnitId{});
}

void TerrainTypeGrid::stamp(Fx x0Elmos, Fx z0Elmos, Fx x1Elmos, Fx z1Elmos,
                            std::uint8_t type, UnitId owner) {
    if (effective_.empty()) {
        return;  // no geometry: a map with no squares has nothing to stamp
    }
    TerrainStamp entry{.owner = owner, .type = type};
    if (!cellRect(x0Elmos, z0Elmos, x1Elmos, z1Elmos, entry)) {
        return;  // entirely off the map — retail's clamped-empty case
    }
    journal_.push_back(entry);
    for (int z = entry.z0; z < entry.z1; ++z) {
        for (int x = entry.x0; x < entry.x1; ++x) {
            effective_[static_cast<std::size_t>(z)
                           * static_cast<std::size_t>(squaresX_)
                       + static_cast<std::size_t>(x)] = type;
        }
    }
    ++version_;
}

void TerrainTypeGrid::restoreJournal(std::span<const TerrainStamp> journal) {
    journal_.assign(journal.begin(), journal.end());
    effective_ = base_;
    rematerialize(0, 0, squaresX_, squaresZ_);
    ++version_;
}

} // namespace rm::sim
