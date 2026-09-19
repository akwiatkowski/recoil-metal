#pragma once

#include "core/map/HeightField.hpp"
#include "core/map/MaxHeightPyramid.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"
#include "core/unit/UnitDef.hpp"
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

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

    /// `Sim::FlattenMapRect` (`C-286`, `0x007524e0`): writes a uniform elevation over a
    /// world-space rect — the sole shipped caller is `StructureUnit.FlattenSkirt`
    /// (`defaultunits.lua:72`), a structure levelling its build site.
    ///
    /// The rect is in ELMOS and covers whole heightmap cells: `floor` on the near edge,
    /// `ceil` on the far, exactly the `math.floor`/`math.ceil` pair the Lua caller applies
    /// to `GetSkirtRect`. Corners at both ends are written, so the flattened area is flat
    /// out to its boundary rather than sloping away inside the last cell.
    ///
    /// Retail's re-seat of Land/Seabed entities in the rect (`CUnitMotion+0x90 = 1`)
    /// needs no counterpart here: `placeOnMotionLayer` re-reads this view every movement
    /// tick, so anything standing in the rect is seated on the new ground next tick.
    /// Retail also queues the rect for the render heightfield (`Sim+0x9f8`/`+0xa08`);
    /// our `TerrainMesh` is baked once at load and has no dirty-rect path, so the
    /// mutation is sim-visible only — matching retail's own choice to leave pathing,
    /// occupancy and (here) the max-height pyramid stale.
    ///
    /// NON-CONST: the one write the sim makes to the map it otherwise only reads.
    void flattenRect(Fx x0Elmos, Fx z0Elmos, Fx x1Elmos, Fx z1Elmos,
                     Fx elevation) noexcept;

    [[nodiscard]] bool hasWater() const noexcept { return hasWater_; }
    [[nodiscard]] Fx waterLevel() const noexcept { return waterLevel_; }

    /// `C-019`'s `STIMap::GetDeepElevation`/`GetAbyssElevation`: the terrain
    /// height clamped to the deep and abyss water levels. A position above
    /// the level answers the level; below it, the ground. Both default to the
    /// water level when the map declares none — retail's own fallback.
    [[nodiscard]] Fx deepHeightAt(Fx x, Fx z) const noexcept;
    [[nodiscard]] Fx abyssHeightAt(Fx x, Fx z) const noexcept;

    /// `C-019`'s `IsPlayable`/`GetPlayableMapRect`: whether a world position
    /// sits inside the map's declared playable area. A map with no declared
    /// rect is playable everywhere.
    [[nodiscard]] bool isPlayable(Fx x, Fx z) const noexcept;
    void setPlayableRect(Fx x0, Fx z0, Fx x1, Fx z1) noexcept;
    void setWaterLevels(float deepElmos, float abyssElmos) noexcept;

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

    /// `C-019`: the deep/abyss water levels and the playable rect. Both
    /// default to "not declared" — deep/abyss fall back to the water level,
    /// and an unset rect means the whole map is playable.
    Fx deepLevel_{};
    Fx abyssLevel_{};
    bool hasDeepLevel_ = false;
    bool hasAbyssLevel_ = false;
    Fx playableX0_{}, playableZ0_{}, playableX1_{}, playableZ1_{};
    bool hasPlayableRect_ = false;
    /// `heightScale * 2^kScaleBits`, in the widening type — not an `Fx`. See `kScaleBits`.
    FxWide heightScale_;
};


/// One recorded write to the terrain-type grid (`C-289`).
///
/// The journal entry, not the diff: the effective grid is base-plus-journal
/// with LAST WRITE WINNING, so a save carries the journal and a revert
/// re-derives the cells underneath rather than restoring a snapshot. `owner`
/// is what makes a write a tarmac — `CreateTarmac`'s stamp lives exactly as
/// long as the structure that laid it, and the tick's sweep lifts it when the
/// owner is gone; a generation-zero owner is a bare `SetTerrainTypeRect`,
/// which nothing reverts.
struct TerrainStamp {
    UnitId owner{};
    /// Cell range in the type grid, half-open [x0,x1) × [z0,z1) — already in
    /// cells, because the journal is what a save replays and the elmo→cell
    /// conversion is the writer's job, done once.
    std::int32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    std::uint8_t type = 0;
};

/// The map's per-square terrain-type grid, plus the runtime writes C-289 adds.
///
/// Retail keeps this on `STIMap` (`+0x1428` cell→typeCode) and mutates it from
/// exactly one place: `SetTerrainTypeRect` (`0x763be0`), which writes the type
/// bytes and — never dirtying the path tables — is why retail commented its
/// sole caller out (`defaultunits.lua:99`: "disabling this for now"). Ours is
/// live: `types()` is the span `buildPassability` reads through the C-288
/// blocking LUT, so a write is visible to pathing and placement the moment a
/// derived grid is rebuilt, and `version()` is the cache-buster that tells a
/// memoised grid its answer is stale.
///
/// THE JOURNAL IS THE STATE. `effective_` is a pure function of the map's
/// base grid and `journal_`, last write winning — which is what makes both
/// directions cheap: a save serializes the journal (a handful of stamps, not
/// a megabyte of cells), and `sweep` reverts a dead owner's stamp by
/// re-deriving only the cells it covered, so a `SetTerrainTypeRect` written
/// UNDER a tarmac shows through again when the tarmac lifts.
class TerrainTypeGrid {
public:
    /// `base` is the map's own type grid (`.scmap`'s `terrainType`, one byte
    /// per square). A span that does not match `squaresX × squaresZ` is
    /// ignored rather than read at a guessed stride — the same refusal
    /// `buildPassability` makes — and the grid then reads as all-default,
    /// which is what an SMF map's absent type grid means anyway.
    /// An empty grid — every `types()` query reads as all-default, which is
    /// what an SMF map's absent type grid means anyway. Needed so `UnitScene`
    /// can hold the grid by value and still default-construct.
    TerrainTypeGrid() = default;

    TerrainTypeGrid(std::span<const std::uint8_t> base, int squaresX,
                    int squaresZ);

    [[nodiscard]] int squaresX() const noexcept { return squaresX_; }
    [[nodiscard]] int squaresZ() const noexcept { return squaresZ_; }
    [[nodiscard]] bool empty() const noexcept { return effective_.empty(); }

    /// The effective type grid — what `buildPassability` and the blocking LUT
    /// read. Empty only when the grid has no geometry at all.
    [[nodiscard]] std::span<const std::uint8_t> types() const noexcept {
        return effective_;
    }

    /// The recorded writes, in order — the serialized form of this state.
    [[nodiscard]] std::span<const TerrainStamp> journal() const noexcept {
        return journal_;
    }

    /// Bumped on every mutation. A cache keyed on the grid's contents
    /// (`PassabilitySet`) compares this rather than re-reading the cells.
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

    /// `Sim::SetTerrainTypeRect` (`C-289`, `0x763be0`): writes `type` over the
    /// squares the elmo rect covers — `floor` on the near edge, `ceil` on the
    /// far, the same pair `FlattenSkirt` applies to `GetSkirtRect`
    /// (defaultunits.lua:70-71). Clamped to the map like retail's rect clamp;
    /// a rect entirely outside writes nothing and records nothing.
    void setRect(Fx x0Elmos, Fx z0Elmos, Fx x1Elmos, Fx z1Elmos,
                 std::uint8_t type);

    /// The same write, owned: a tarmac's stamp, which `sweep` lifts when the
    /// owning unit is gone. The owner is journaled so a save keeps the
    /// revert contract, not just the bytes.
    void stamp(Fx x0Elmos, Fx z0Elmos, Fx x1Elmos, Fx z1Elmos,
               std::uint8_t type, UnitId owner);

    /// Drops every stamp whose owner fails `alive` — the `DestroyTarmac`
    /// counterpart (defaultunits.lua:159), run by the tick so a death, a
    /// reclaim, a capture-kill and an upgrade replace all revert the same
    /// way. Re-derives only the cells the lifted stamps covered.
    template <typename Alive>
    void sweep(Alive&& alive) {
        int x0 = squaresX_, z0 = squaresZ_, x1 = 0, z1 = 0;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < journal_.size(); ++i) {
            const TerrainStamp& entry = journal_[i];
            if (entry.owner.generation != 0 && !alive(entry.owner)) {
                x0 = std::min(x0, entry.x0);
                z0 = std::min(z0, entry.z0);
                x1 = std::max(x1, entry.x1);
                z1 = std::max(z1, entry.z1);
                continue;
            }
            journal_[kept++] = entry;
        }
        if (kept == journal_.size()) {
            return;
        }
        journal_.resize(kept);
        rematerialize(x0, z0, x1, z1);
        ++version_;
    }

    /// Replays a serialized journal — the restore half of the save contract.
    /// Entries apply in order over the base grid, exactly as if the writes
    /// had been made live.
    void restoreJournal(std::span<const TerrainStamp> journal);

private:
    /// Recomputes `effective_` over the cell rect from base plus journal —
    /// last write wins, so a cell's type is the newest entry covering it, or
    /// the map's own byte where none does.
    void rematerialize(int x0, int z0, int x1, int z1) noexcept;

    /// The elmo→cell conversion shared by `setRect` and `stamp`: floor on the
    /// near edge, ceil on the far, clamped to the grid. Returns false when
    /// the rect misses the map entirely.
    [[nodiscard]] bool cellRect(Fx x0Elmos, Fx z0Elmos, Fx x1Elmos,
                                Fx z1Elmos, TerrainStamp& out) const noexcept;

    int squaresX_ = 0;
    int squaresZ_ = 0;
    std::vector<std::uint8_t> base_;
    std::vector<std::uint8_t> effective_;
    std::vector<TerrainStamp> journal_;
    std::uint64_t version_ = 0;
};
} // namespace rm::sim
