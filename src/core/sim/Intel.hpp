#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rm::sim {

class Terrain;

// What an alliance can see, and by what means (ADR-037).
//
// WHY THIS EXISTS. Nothing in this engine knew: `nearestTarget` picked from the whole unit
// store filtered by hostility and range, so every unit shot at things it could not possibly
// see, and the minimap said "no fog of war" in a comment because there was none to draw.
//
// THE TWO ENGINES DISAGREE ABOUT WHAT THE FEATURE IS, which is the fact that shaped this
// file. Recoil raycasts terrain for sight and radar (`LosMap.cpp:525`, an angle/horizon scan)
// and takes circles for everything else. Supreme Commander does not consider terrain at all:
// `effects/vision.fx:35` is `radius * vertex.xz + position.xy`, a flat disc with no height
// input and no heightmap sample anywhere in the file. So the SHAPE is a parameter here and
// the grid underneath it is common — see `VisionStyle`.
//
// THE STRUCTURE IS RECOIL'S AND THE IMPORTANT PART IS SMALLER THAN THE RAYCAST: a square
// holds a **reference count, not a flag** (`ILosType`, LosHandler.h:84-88). Withdrawal is
// most of what an intel system does — units move every tick — and with a flag one unit's
// sight cannot be taken back without re-deriving every other unit's. With a count, add and
// remove are +1 and -1 over the same squares and each unit's contribution is independent.
//
// PER ALLIANCE, not per army, because `Army.hpp` already named `AllianceIndex` as "who wins
// together, and later who shares vision". This is later.

/// The senses this engine models, and the order they are stored in.
///
/// THREE, NOT FOUR. `UnitDef` also carries a water vision radius, which Forged Alliance
/// treats as its own sense — but nothing in this sim is submerged yet, so a fourth grid
/// would have no target to answer about and no test that could tell it from an empty one.
/// It joins these when there is something under the water to see.
///
/// Recoil's other four types — air LOS, seismic, and the two jammers — are absent for the
/// same reason: air LOS needs a distinction between flying and grounded units that the
/// coverage query does not yet make, and a jammer needs the contact rules of ADR-037's
/// deferred half.
enum class IntelKind : std::uint8_t {
    Vision,
    Radar,
    Sonar,
};

inline constexpr std::size_t kIntelKindCount = 3;

/// Whether terrain blocks sight, which is a question about which game this is.
enum class VisionStyle : std::uint8_t {
    /// Flat discs for everything, as `vision.fx` stamps them. You see over mountains.
    ForgedAlliance,

    /// Sight and radar are raycast against the ground; sonar stays a disc, as it is in
    /// `LosHandler.cpp:92`. Hills block, and high ground is worth taking.
    Recoil,
};

/// Elmos across one square at mip 0 — Recoil's `SQUARE_SIZE`, and the resolution its
/// heightmap is stated in. A grid's own square is this shifted left by its mip level, the
/// same `mipDiv = SQUARE_SIZE * (1 << mipLevel)` (`LosHandler.cpp:87`).
inline constexpr std::int32_t kElmosPerSquare = 8;

// One alliance's coverage of one sense: a reference count per square.
//
// SIZED IN SQUARES, NOT ELMOS, and every operation below works in square coordinates. That
// is Recoil's choice too and the reason is arithmetic rather than memory: a circle
// rasterised in integer squares is the same circle on every machine, whereas one rasterised
// against fixed-point elmos would depend on where the rounding fell.
class IntelGrid {
public:
    /// What `squareAt` answers for a position that is not on the map.
    ///
    /// Not the nearest square. Clamping would give a unit standing past the border sight
    /// inside it, and "off the map" is a real state during a spawn.
    static constexpr std::int32_t kNoSquare = -1;

    IntelGrid() = default;

    /// `mipLevel` coarsens the grid: 0 is one square per 8 elmos, 2 is one per 32.
    ///
    /// Recoil keeps sight and radar at different levels (`modInfo.losMipLevel` against
    /// `radarMipLevel`) because radar radii are an order larger — the widest in the retail
    /// corpus is 4800 elmos against the widest sight of 800 — and rasterising that at sight's
    /// resolution costs 36x the squares for a sense whose edge nobody can see.
    IntelGrid(Fx widthElmos, Fx depthElmos, int mipLevel);

    [[nodiscard]] int squaresX() const noexcept { return squaresX_; }
    [[nodiscard]] int squaresZ() const noexcept { return squaresZ_; }
    [[nodiscard]] Fx squareElmos() const noexcept { return Fx::fromInt(squareElmos_); }

    /// The square under a world position, or `kNoSquare` off the map.
    [[nodiscard]] std::int32_t squareAt(Fx x, Fx z) const noexcept;

    /// +1 and -1 over every square of a coverage shape.
    ///
    /// The shape is a caller-owned list of square indices — `circleSquares` or
    /// `raycastSquares` fills one — rather than something this class computes, because the
    /// same grid serves both algorithms and because a caller that has to hold the shape
    /// anyway is a caller that can withdraw exactly what it added.
    void add(std::span<const std::int32_t> squares) noexcept;
    void remove(std::span<const std::int32_t> squares) noexcept;

    [[nodiscard]] bool covered(std::int32_t square) const noexcept {
        return square != kNoSquare && counts_[static_cast<std::size_t>(square)] != 0;
    }

    [[nodiscard]] bool covered(Fx x, Fx z) const noexcept { return covered(squareAt(x, z)); }

    /// How many emitters count this square. For tests and for the hash; a caller asking
    /// "can I see this" wants `covered`.
    [[nodiscard]] std::uint16_t count(std::int32_t square) const noexcept {
        return square == kNoSquare ? 0 : counts_[static_cast<std::size_t>(square)];
    }

    /// The whole grid, in row-major square order. What `StateHash` feeds and what a fog
    /// renderer uploads.
    [[nodiscard]] std::span<const std::uint16_t> counts() const noexcept { return counts_; }

private:
    int squaresX_ = 0;
    int squaresZ_ = 0;
    std::int32_t squareElmos_ = kElmosPerSquare;

    /// A REFERENCE COUNT, sized to hold every unit in a match standing on one square.
    /// `uint16` is 65,535 of them; the largest match this engine targets is thousands.
    std::vector<std::uint16_t> counts_;
};

/// The squares a flat disc of `radius` about (x, z) covers, appended to `squares`.
///
/// Forged Alliance's whole vision model, and Recoil's for the senses it does not raycast.
/// The rasteriser is Recoil's midpoint circle (`LosMap.cpp:74-103`) — integer arithmetic
/// only, and it emits each row exactly once, which matters because a duplicated row would
/// double every count in it. Clipped at the map edge rather than wrapped.
///
/// A radius under one square still covers the square the emitter stands on: 8 blueprints
/// state a sight radius that small, and a unit that cannot see its own feet is a worse
/// reading of the file than one that sees a single square.
void circleSquares(const IntelGrid& grid, Fx x, Fx z, Fx radius,
                   std::vector<std::int32_t>& squares);

/// The squares an emitter at `eyeHeight` can see over the ground, appended to `squares`.
///
/// Recoil's LOS, and the shape of it is simpler than its reputation: **start from the disc
/// and subtract what the ground hides.** Rays are cast outward from the centre; along each
/// one a running maximum angle rises as the ray climbs, and a square whose own angle falls
/// below that maximum is behind a crest and gets struck off.
///
/// Transcribed from `LosMap.cpp` — `CastLos` at 525, the angle definition at 654, the ray
/// construction at 181-309 — with four differences worth stating rather than hiding:
///
///   - **Heights are sampled, not mipped.** Recoil reads a pre-reduced heightmap at the
///     grid's own mip level; we sample the real terrain at each square's centre. Identical
///     at mip 0 and an approximation above it, in the direction of detail rather than away.
///   - **No angle table and no instance cache.** Both are Recoil making the same
///     computation cheap across thousands of units; ours runs on `SlowUpdate`'s cadence and
///     only for emitters that moved.
///   - **The eye height is not bucketed.** Recoil rounds it into buckets so two units at
///     similar heights can share one cached instance. We share nothing, so rounding the
///     input would lose accuracy and buy nothing.
///   - **Fixed point throughout**, where Recoil uses floats and a reciprocal-square-root
///     table. `fxSqrt` is exact on every machine and a `libm` call is not (`Fx.hpp`).
///
/// The units of the angle are mixed on purpose, exactly as the original's are: a height
/// difference in ELMOS over a distance in SQUARES. Every comparison happens within one
/// grid, so the scale cancels — but it does mean the sight bonus below is worth more on a
/// coarse grid than a fine one, which is Recoil's behaviour too.
void raycastSquares(const IntelGrid& grid, const Terrain& terrain, Fx x, Fx z, Fx radius,
                    Fx eyeHeight, std::vector<std::int32_t>& squares);

/// What `kind` covers under `style`: the dispatch ADR-037's setting comes down to.
///
/// Recoil raycasts sight and radar and leaves sonar a disc (`LosHandler.cpp:92`); Forged
/// Alliance stamps discs for all three. A null `terrain` is a scene with no ground to
/// consult — a `--units` crowd on procedural terrain — and falls back to the disc, which is
/// the honest answer rather than a raycast against a heightmap that is not there.
void intelSquares(const IntelGrid& grid, const Terrain* terrain, VisionStyle style,
                  IntelKind kind, Fx x, Fx z, Fx radius, Fx eyeHeight,
                  std::vector<std::int32_t>& squares);

} // namespace rm::sim
