#pragma once

#include "core/map/HeightField.hpp"
#include "core/map/MaxHeightPyramid.hpp"
#include "core/sim/Fx.hpp"
#include "core/unit/UnitDef.hpp"
#include <span>

namespace rm::sim {

struct ResourceDeposit {
    unitdef::BuildRestriction kind = unitdef::BuildRestriction::MassDeposit;
    Fx x{}, z{};
};

/// Where a structure may stand.
///
/// GRID is the game's rule and the default: Supreme Commander places structures on the
/// one-ogrid build grid so that skirts abut exactly — a power generator's edge meets the
/// factory's, a storage's meets the extractor's — and adjacency is a matter of placement,
/// not luck. An even footprint centres on a grid line, an odd one on a cell centre, which is
/// also where every retail deposit sits (`SCMP_009_save.lua`: 346.5, 678.5 ...).
///
/// FREE keeps the exact ordered coordinate. It is what this engine did before the grid
/// existed and stays available for comparison and for tests that reason about distances.
enum class PlacementMode : std::uint8_t { Grid, Free };

/// One ogrid in elmos — the build grid's pitch.
inline constexpr Fx kBuildGridElmos = Fx::fromInt(8);

/// Snaps one axis of a structure's centre to the build grid for a footprint of
/// `footprintSquares` ogrids along that axis: even footprints land on grid lines, odd ones on
/// cell centres. A footprint of zero (unread) is treated as one.
[[nodiscard]] Fx snapToBuildGrid(Fx centre, int footprintSquares) noexcept;


// The ground, as the sim sees it: fixed point in, fixed point out.
//
// WHY A SEPARATE VIEW. `HeightField::heightAtWorld` takes and returns `float`, and it should —
// it is the map, it is loaded from a file that states floats, and the terrain mesh builder and
// the camera both want floats. But a sim pass that called it would put a float in the middle
// of the tick, which is the one thing `Fx.hpp` exists to prevent.
//
// The trick that makes this cheap: **the raw grid is already integers.** A height is
// `baseHeight + raw * heightScale` with `raw` a `uint16`, and that decode is affine — so
// converting `baseHeight` and `heightScale` to `Fx` ONCE, at construction, makes every
// subsequent sample pure integer arithmetic over data that was already integral. No parallel
// array, no quantisation of the grid, no memory cost: two `Fx` values and the field's own
// `raw`.
//
// (A quantised copy of the grid was the obvious alternative and is the wrong one. The biggest
// shipped map is 4097 x 4097 samples; at four bytes each that is 67 MB of duplicate terrain
// to keep in step with the original.)
//
// Bilinear, matching `heightAtWorld` exactly in shape so the two cannot disagree about where
// the ground is — one in float for the renderer, one in fixed point for the sim, same
// arithmetic.
class Terrain {
public:
    /// Holds a REFERENCE to the field. The field outlives the sim in every caller — it is the
    /// map — and copying a heightfield to sample it would be absurd.
    explicit Terrain(const HeightField& field, bool hasWater = false,
                     float waterLevelElmos = 0.0f,
                     const MaxHeightPyramid* lookAhead = nullptr,
                     std::span<const ResourceDeposit> deposits = {},
                     PlacementMode placement = PlacementMode::Grid) noexcept;

    [[nodiscard]] bool resourceSitePlaceable(unitdef::BuildRestriction restriction,
                                             Fx x, Fx z) const noexcept;

    [[nodiscard]] PlacementMode placement() const noexcept { return placement_; }

    /// The site a structure order at (`x`, `z`) actually claims: the coordinate itself in
    /// Free mode; in Grid mode the footprint-aligned grid point, except that a deposit-bound
    /// structure keeps the deposit's own centre, which is authoritative.
    [[nodiscard]] std::array<Fx, 2> buildSite(const unitdef::UnitDef& def, Fx x,
                                              Fx z) const noexcept;

    /// The height at a grid corner, clamped at the edges.
    ///
    /// Clamped rather than wrapped, matching `HeightField::heightAt`: a sample past the border
    /// mirrors the edge, which yields the correct flat-continuation behaviour rather than
    /// needing an edge case at every border vertex.
    [[nodiscard]] Fx cornerHeight(std::int32_t x, std::int32_t z) const noexcept;

    /// The interpolated height under a world position. The sim's `heightAtWorld`.
    [[nodiscard]] Fx heightAt(Fx x, Fx z) const noexcept;

    /// The surface under a world position: the ground, or the water where the ground is
    /// drowned. What an aircraft measures its height above and lands on (`C-222`).
    [[nodiscard]] Fx surfaceHeightAt(Fx x, Fx z) const noexcept;

    /// The highest surface a flyer must clear within `reachElmos` of a position — retail's
    /// terrain look-ahead (`C-246`). Not a directional scan: retail indexes a max-height
    /// pyramid at the level whose cell is at least half the reach wide, so the answer is the
    /// maximum over the power-of-two cell CONTAINING the position, aligned to the grid.
    /// Below one ogrid of reach it is the point sample. Water counts as surface.
    ///
    /// One load from the map's `MaxHeightPyramid` when the view was given one and the
    /// vertical scale is positive; otherwise the cell's corners are scanned, which answers
    /// identically and costs `(2^L + 1)^2` reads.
    [[nodiscard]] Fx maxSurfaceHeightNear(Fx x, Fx z, Fx reachElmos) const noexcept;

    /// The field this samples, for the passes that still need its integer geometry — square
    /// counts, extents. Deliberately not a way back to the float accessors: those are the
    /// renderer's.
    [[nodiscard]] const HeightField& field() const noexcept { return *field_; }

    [[nodiscard]] bool hasWater() const noexcept { return hasWater_; }
    [[nodiscard]] Fx waterLevel() const noexcept { return waterLevel_; }

    /// How many fractional bits the vertical scale is kept to. **Thirty, not fourteen.**
    ///
    /// This is the one number in the file that needed measuring rather than assuming. A real
    /// `.smf` states a scale on the order of 0.01 elmos per raw unit; in `Q18.14` that
    /// quantises to 164/16384 = 0.010009765625, a relative error of one part in a thousand.
    /// Harmless on its own — and then multiplied by a `raw` of up to 65,535, which amplifies
    /// it into an error of a sixth of an elmo. Measured against the float accessor: 0.17
    /// elmos, seven hundred times the type's own resolution.
    ///
    /// Keeping the scale to 2^-30 makes the error in `raw * scale` at most 6.5e-5 — one step
    /// of the result — which is where it belongs. The general lesson, worth stating because it
    /// will come up again: **a small factor multiplied by a large operand needs more
    /// fractional bits than the product does.**
    static constexpr int kScaleBits = 30;

private:
    /// One raw word decoded to elmos: `baseHeight + raw × scale`, the affine map every
    /// sample shares (see `kScaleBits`).
    [[nodiscard]] Fx decodeRaw(std::uint16_t raw) const noexcept;

    std::span<const ResourceDeposit> deposits_;
    PlacementMode placement_ = PlacementMode::Grid;
    const HeightField* field_;
    const MaxHeightPyramid* lookAhead_ = nullptr;
    Fx baseHeight_;
    bool hasWater_ = false;
    Fx waterLevel_{};

    /// `heightScale * 2^kScaleBits`, in the widening type — not an `Fx`. See `kScaleBits`.
    FxWide heightScale_;
};

} // namespace rm::sim
