#pragma once

#include "core/map/HeightField.hpp"
#include "core/sim/Fx.hpp"

namespace rm::sim {

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
                     float waterLevelElmos = 0.0f) noexcept;

    /// The height at a grid corner, clamped at the edges.
    ///
    /// Clamped rather than wrapped, matching `HeightField::heightAt`: a sample past the border
    /// mirrors the edge, which yields the correct flat-continuation behaviour rather than
    /// needing an edge case at every border vertex.
    [[nodiscard]] Fx cornerHeight(std::int32_t x, std::int32_t z) const noexcept;

    /// The interpolated height under a world position. The sim's `heightAtWorld`.
    [[nodiscard]] Fx heightAt(Fx x, Fx z) const noexcept;

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
    const HeightField* field_;
    Fx baseHeight_;
    bool hasWater_ = false;
    Fx waterLevel_{};

    /// `heightScale * 2^kScaleBits`, in the widening type — not an `Fx`. See `kScaleBits`.
    FxWide heightScale_;
};

} // namespace rm::sim
