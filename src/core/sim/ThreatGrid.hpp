#pragma once

// One army's influence map: what it believes is dangerous, and where (C-355/C-356/C-357).
//
// RETAIL'S OBJECT. `CArmyImpl+0x218` owns a `CInfluenceMap`: a flat cell array where each
// cell (`InfluenceGrid`, ~0x8c bytes) holds a `map<ReconBlip*,InfluenceMapEntry>` of
// per-contact contributions, a per-army `vector<SThreat>`, an unassigned `SThreat`, and
// per-type decay rates. `SThreat` is `float[14]` — here `Mag[14]`, because this sim's
// arithmetic is fixed point (PLAN2 §5.2) and threat is a magnitude, not a geometry.
//
// THE CADENCE IS THE FEATURE. `CArmyImpl::Update` (`0x70686a`–`0x706891`, magic-divide
// `0x88888889`) runs the distribute pass `0x71cf00` only when `tick % 30 == armyIndex`:
// every army re-derives its grid once per three seconds at 10 Hz, staggered so no two
// armies pay for it on the same tick. Between passes the grid is STALE BY DESIGN — a
// contact that dies or moves keeps contributing where it was last seen until the army's
// next pass sweeps it. That is the behaviour the queries below expose, and the reason a
// killed scout's report outlives the scout.
//
// WHAT THE PASS DOES (`0x71cf00`, per cell): iterates the cell's blip map, loads the four
// threat levels cached on the blip at `blip+0x42c..0x438` — the blueprint's
// `Air/Surface/Sub/EconomyThreatLevel` (`blueprint_schema.tsv` 661-664) — multiplies each
// by a per-blip weight, sums all four into `f[0]`/`f[1]` (the maintained totals), and
// distributes the total plus weighted components into typed slots: the blip's TOTAL into
// `f[9]` Artillery, `f[6]` Air, `f[7]` Experimental, `f[8]` Commander or `f[2]`
// StructuresNotMex as its categories dictate; the COMPONENTS into `f[12]` AntiSub,
// `f[11]` AntiAir, `f[10]` AntiSurface and `f[3]` Structures (the economy component —
// `SThreat` has no Economy slot; see `ThreatSlot`). The per-blip weight `xmm1`
// (`0x71d169`) is unresolved in the findings — likely a veterancy or TTL scale — so every
// blip here weighs 1.0 until that read lands.
//
// WHO CONTRIBUTES. Retail's cells key entries by `ReconBlip*`, so a contact contributes
// under the army that OWNS it and `InfluenceGrid::GetThreat(type, army)` can answer
// "threat from army N" (`army<0` sums across armies). Our blips are keyed by `UnitId`
// with the owner recorded, fed from the opponent's observed snapshot: own and allied
// units are always known, enemies only while vision covers them — the same filter the
// snapshot's `enemies` list applies.

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace rm::sim {

/// The `SThreat` slot indices — retail's `EThreatType` order as the claim ledger records
/// it (C-355/C-356), NOT the `enum_registrations.tsv` value column, which lists a 15th
/// type (`Economy`) the 14-float record cannot hold. The distribute pass's writes are
/// confirmed per slot; `Naval`/`Land` receive no blip contribution in the disassembly and
/// stay queryable-but-empty, matching retail.
enum class ThreatSlot : std::uint8_t {
    Overall = 0,           // maintained sum of every blip's four levels
    OverallNotAssigned = 1,// second maintained total the pass writes identically
    StructuresNotMex = 2,  // total of non-extractor structures
    Structures = 3,        // the ECONOMY component of every blip
    Naval = 4,             // never written by the pass; queries read 0
    Land = 5,              // never written by the pass; queries read 0
    Air = 6,               // total of AIR-category blips
    Experimental = 7,      // total of EXPERIMENTAL blips
    Commander = 8,         // total of COMMAND blips
    Artillery = 9,         // total of ARTILLERY blips
    AntiSurface = 10,      // the surface component of every blip
    AntiAir = 11,          // the air component of every blip
    AntiSub = 12,          // the sub component of every blip
    Unknown = 13,          // AssignThreatAtPosition's invalid-type bucket (C-357)
};

inline constexpr std::size_t kThreatSlotCount = 14;

/// One cell's threat record: `float f[14]` in retail, `Mag` here.
using SThreat = std::array<Mag, kThreatSlotCount>;

/// Maps a Lua `threatType` string to a slot. `std::nullopt` for absent/unknown spellings —
/// queries treat that as `Overall` (retail's default read), assigns as `Unknown` (C-357).
/// 'Economy' resolves to `Structures`: the retail enum's 15th name has no slot in the
/// 14-float record, and the economy component is exactly what `f[3]` accumulates.
[[nodiscard]] std::optional<ThreatSlot> threatSlotFor(std::string_view threatType) noexcept;

/// One observed contact's contribution — the `InfluenceMapEntry` (`0x71d117`):
/// identity, owner, cell, the four cached blueprint threat levels, and the category
/// flags the distribute pass's slot routing reads.
struct ThreatBlip {
    UnitId unit{};
    int owner = -1;             // source army index; -1 = unowned
    std::int32_t cell = -1;     // grid cell the contact was last observed in
    Mag surface{};              // blueprint SurfaceThreatLevel  -> f[10]
    Mag air{};                  // blueprint AirThreatLevel      -> f[11]
    Mag sub{};                  // blueprint SubThreatLevel      -> f[12]
    Mag economy{};              // blueprint EconomyThreatLevel  -> f[3]
    bool isAir = false;         // AIR category      -> total into f[6]
    bool isExperimental = false;// EXPERIMENTAL      -> total into f[7]
    bool isCommander = false;   // COMMAND           -> total into f[8]
    bool isArtillery = false;   // ARTILLERY         -> total into f[9]
    bool isStructure = false;   // STRUCTURE         -> total into f[2] unless extractor
    bool isExtractor = false;   // MASSEXTRACTION    -> excluded from f[2]
    bool seen = false;          // refreshed by observe(); swept by the next distribute
};

/// One row of `GetThreatsAroundPosition`: a cell's centre and its summed threat (C-357 —
/// retail emits `{x, z, threat}` triples per CELL, `0x597a41`–`0x597a71`).
struct ThreatRow {
    Fx x{};
    Fx z{};
    Mag threat{};
};

/// The per-army grid. Configured once per match; `observe` refreshes contacts every
/// decision pass; `distribute` runs on the 30-tick stagger and is the ONLY writer of the
/// cell aggregates queries read.
class ThreatGrid {
public:
    /// What `cellAt` answers off the map — `IntelGrid::kNoSquare`'s convention.
    static constexpr std::int32_t kNoCell = -1;

    ThreatGrid() = default;

    /// Sizes the grid. `cellElmos` is the brain's IMAP cell in elmos — the same value
    /// `IMAPConfig.IMAPSize * 8` produces — so `rings` in `threatAt` means the same cells
    /// retail's query walks. Idempotent; a reconfigure is a new match.
    void configure(Fx widthElmos, Fx depthElmos, Fx cellElmos);

    [[nodiscard]] bool active() const noexcept { return cellElmos_ > 0; }
    [[nodiscard]] int cellsX() const noexcept { return cellsX_; }
    [[nodiscard]] int cellsZ() const noexcept { return cellsZ_; }
    [[nodiscard]] Fx cellElmos() const noexcept { return Fx::fromInt(cellElmos_); }

    /// The cell under a world position, or `kNoCell` off the map.
    [[nodiscard]] std::int32_t cellAt(Fx x, Fx z) const noexcept;

    /// World position of a cell's centre — what `ThreatRow` reports.
    [[nodiscard]] std::array<Fx, 2> cellCentre(std::int32_t cell) const noexcept;

    /// Refreshes one contact's contribution. Called every pass for every unit the army
    /// currently knows about; a blip NOT re-observed keeps its last cell until the next
    /// `distribute` sweeps it — the decay the stagger exists to provide.
    void observe(const ThreatBlip& blip);

    /// `AssignThreatAtPosition`: deposits `threat` into the cell's unassigned record at
    /// `slot`, decaying by `decay` per distribute pass (C-357: invalid/absent types land
    /// in `Unknown`). Retail stores one decay rate per cell per slot; a second assign to
    /// the same slot overwrites the rate, as the single `decay` field implies.
    void assign(Fx x, Fx z, Mag threat, Mag decay, ThreatSlot slot);

    /// The staggered pass (`0x71cf00`): rebuilds every cell's per-army `SThreat` from the
    /// blips still seen, sweeps blips no pass re-observed, and decays assigned deposits.
    void distribute();

    /// `GetThreatAtPosition`: the cell's threat of `type`, summed over `rings` of
    /// Chebyshev neighbours. `army < 0` sums every contributor plus unassigned deposits;
    /// `army >= 0` reads that army's record only (retail's `GetThreat` jump table,
    /// `0x71c1d0`/`0x71c600`).
    [[nodiscard]] Mag threatAt(Fx x, Fx z, int rings, ThreatSlot type, int army) const noexcept;

    /// `GetThreatsAroundPosition`: one row per cell whose centre lies within `radius`
    /// elmos, appended in row-major cell order (retail's walker order, `0x71ca70`).
    void threatsAround(Fx x, Fx z, Fx radius, ThreatSlot type, int army,
                       std::vector<ThreatRow>& out) const;

    /// `GetThreatBetweenPositions`: the sum over the cells the segment crosses (C-357's
    /// scalar variant — `addss` accumulation along the walk).
    [[nodiscard]] Mag threatBetween(Fx ax, Fx az, Fx bx, Fx bz, ThreatSlot type,
                                    int army) const noexcept;

    /// `GetHighestThreatPosition`: the cell whose `rings`-neighbourhood sum is largest,
    /// and that sum. `std::nullopt` on an empty grid — the corpus only consumes the
    /// position when threat exists (`aiutilities.lua:545`).
    [[nodiscard]] std::optional<std::pair<std::array<Fx, 2>, Mag>> highestThreat(
        int rings, ThreatSlot type, int army) const noexcept;

private:
    /// One cell: per-source-army records plus the unassigned bucket and its decay rates —
    /// `InfluenceGrid`'s `vector<SThreat>` at `+0x0c`, `SThreat` at `+0x1c`, and the decay
    /// `SThreat` at `+0x54`.
    struct Cell {
        std::map<int, SThreat> byArmy;
        SThreat unassigned{};
        SThreat decay{};
    };

    /// Adds one blip's weighted contribution into `record` — the distribute pass's inner
    /// write (`0x71d1b5`–`0x71d405`): totals into `f[0]`/`f[1]`, the total into the
    /// category slots, each component into its Anti*/Structures slot.
    static void addBlip(SThreat& record, const ThreatBlip& blip) noexcept;

    /// The cell's threat of `type` for `army` (`army < 0` = all armies + unassigned).
    [[nodiscard]] Mag cellThreat(const Cell& cell, ThreatSlot type, int army) const noexcept;

    int cellsX_ = 0;
    int cellsZ_ = 0;
    std::int32_t cellElmos_ = 0;
    std::int32_t widthElmos_ = 0;
    std::int32_t depthElmos_ = 0;
    std::vector<Cell> cells_;
    /// Contacts by packed `UnitId` (generation high, index low — `FafOpponent::packHandle`'s
    /// packing, so a Lua handle and a grid key agree). A map, not a vector: blips arrive in
    /// slot order anyway, and the key survives slot reuse exactly like the handle does.
    std::map<std::uint64_t, ThreatBlip> blips_;
};

} // namespace rm::sim
