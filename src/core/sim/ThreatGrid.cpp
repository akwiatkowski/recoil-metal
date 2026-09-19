#include "core/sim/ThreatGrid.hpp"

#include <algorithm>
#include <cmath>

namespace rm::sim {

namespace {

/// Case-insensitive compare for the threat-type strings the corpus spells every way it
/// can ('AntiSurface', 'antisurface', …). Blueprint categories are uppercase by
/// convention but nothing enforces it, and a case-sensitive miss would silently route
/// 'Unknown' queries to Overall.
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto upper = [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        };
        if (upper(a[i]) != upper(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint64_t packBlipKey(UnitId id) noexcept {
    return (std::uint64_t{id.generation} << 32) | std::uint64_t{id.index};
}

} // namespace

std::optional<ThreatSlot> threatSlotFor(std::string_view threatType) noexcept {
    // The retail `EThreatType` names (C-355). 'Economy' has no slot in the 14-float
    // record — see ThreatSlot — and reads as Structures, the slot its component feeds.
    if (iequals(threatType, "Overall")) return ThreatSlot::Overall;
    if (iequals(threatType, "OverallNotAssigned")) return ThreatSlot::OverallNotAssigned;
    if (iequals(threatType, "StructuresNotMex")) return ThreatSlot::StructuresNotMex;
    if (iequals(threatType, "Structures")) return ThreatSlot::Structures;
    if (iequals(threatType, "Economy")) return ThreatSlot::Structures;
    if (iequals(threatType, "Naval")) return ThreatSlot::Naval;
    if (iequals(threatType, "Land")) return ThreatSlot::Land;
    if (iequals(threatType, "Air")) return ThreatSlot::Air;
    if (iequals(threatType, "Experimental")) return ThreatSlot::Experimental;
    if (iequals(threatType, "Commander")) return ThreatSlot::Commander;
    if (iequals(threatType, "Artillery")) return ThreatSlot::Artillery;
    if (iequals(threatType, "AntiSurface")) return ThreatSlot::AntiSurface;
    if (iequals(threatType, "AntiAir")) return ThreatSlot::AntiAir;
    if (iequals(threatType, "AntiSub")) return ThreatSlot::AntiSub;
    if (iequals(threatType, "Unknown")) return ThreatSlot::Unknown;
    return std::nullopt;
}

void ThreatGrid::configure(Fx widthElmos, Fx depthElmos, Fx cellElmos) {
    cells_.clear();
    blips_.clear();
    cellElmos_ = cellElmos.floorToInt();
    widthElmos_ = widthElmos.floorToInt();
    depthElmos_ = depthElmos.floorToInt();
    if (cellElmos_ <= 0 || widthElmos_ <= 0 || depthElmos_ <= 0) {
        cellsX_ = cellsZ_ = 0;
        return;
    }
    cellsX_ = (widthElmos_ + cellElmos_ - 1) / cellElmos_;
    cellsZ_ = (depthElmos_ + cellElmos_ - 1) / cellElmos_;
    cells_.resize(static_cast<std::size_t>(cellsX_) * static_cast<std::size_t>(cellsZ_));
}

std::int32_t ThreatGrid::cellAt(Fx x, Fx z) const noexcept {
    if (!active()) {
        return kNoCell;
    }
    const int gx = x.floorToInt() / cellElmos_;
    const int gz = z.floorToInt() / cellElmos_;
    if (gx < 0 || gz < 0 || gx >= cellsX_ || gz >= cellsZ_) {
        return kNoCell;
    }
    return gz * cellsX_ + gx;
}

std::array<Fx, 2> ThreatGrid::cellCentre(std::int32_t cell) const noexcept {
    const int gx = cell % cellsX_;
    const int gz = cell / cellsX_;
    // The centre of the cell's COVERED portion: an edge cell smaller than cellElmos_
    // (a map that does not divide evenly, or a test map smaller than one cell) reports
    // the middle of what is actually on the map, not a point outside it.
    return {Fx::fromInt(gx * cellElmos_ + std::min(cellElmos_, widthElmos_ - gx * cellElmos_) / 2),
            Fx::fromInt(gz * cellElmos_ + std::min(cellElmos_, depthElmos_ - gz * cellElmos_) / 2)};
}

void ThreatGrid::observe(const ThreatBlip& blip) {
    ThreatBlip& entry = blips_[packBlipKey(blip.unit)];
    entry = blip;
    entry.seen = true;
}

void ThreatGrid::assign(Fx x, Fx z, Mag threat, Mag decay, ThreatSlot slot) {
    const std::int32_t cell = cellAt(x, z);
    if (cell == kNoCell) {
        return;
    }
    Cell& c = cells_[static_cast<std::size_t>(cell)];
    const auto index = static_cast<std::size_t>(slot);
    c.unassigned[index] += threat;
    // One decay rate per cell per slot (InfluenceGrid's single decay SThreat at +0x54):
    // a repeated assign refreshes the rate rather than stacking it.
    c.decay[index] = decay;
}

void ThreatGrid::distribute() {
    // The pass `0x71cf00`: rebuild the per-army records from live blips, sweep the ones
    // no pass re-observed, and decay the unassigned bucket by its per-slot rates.
    for (Cell& cell : cells_) {
        cell.byArmy.clear();
        for (std::size_t i = 0; i < kThreatSlotCount; ++i) {
            cell.unassigned[i] -= cell.decay[i];
            if (cell.unassigned[i] < Mag{}) {
                cell.unassigned[i] = Mag{};
            }
        }
    }
    for (auto it = blips_.begin(); it != blips_.end();) {
        ThreatBlip& blip = it->second;
        if (!blip.seen || blip.cell == kNoCell
            || static_cast<std::size_t>(blip.cell) >= cells_.size()) {
            // Dead or moved out of contact: the contribution vanishes on THIS pass, not
            // the tick it happened — the whole point of the 30-tick stagger (C-356).
            it = blips_.erase(it);
            continue;
        }
        addBlip(cells_[static_cast<std::size_t>(blip.cell)].byArmy[blip.owner], blip);
        blip.seen = false;  // must be re-earned by the next observation round
        ++it;
    }
}

void ThreatGrid::addBlip(SThreat& record, const ThreatBlip& blip) noexcept {
    // The per-blip weight `xmm1` (`0x71d169`) is unresolved — likely a veterancy or TTL
    // scale — so every component weighs 1.0 here.
    const Mag total = blip.surface + blip.air + blip.sub + blip.economy;
    record[static_cast<std::size_t>(ThreatSlot::Overall)] += total;
    record[static_cast<std::size_t>(ThreatSlot::OverallNotAssigned)] += total;
    // Aggregate slots take the blip's TOTAL (`0x71d27b`–`0x71d405`): a blip contributes
    // to every category slot it qualifies for — a commander is both COMMAND and, on most
    // blueprints, carries no ARTILLERY, so the flags are independent adds, not a switch.
    if (blip.isArtillery) {
        record[static_cast<std::size_t>(ThreatSlot::Artillery)] += total;
    }
    if (blip.isAir) {
        record[static_cast<std::size_t>(ThreatSlot::Air)] += total;
    }
    if (blip.isExperimental) {
        record[static_cast<std::size_t>(ThreatSlot::Experimental)] += total;
    }
    if (blip.isCommander) {
        record[static_cast<std::size_t>(ThreatSlot::Commander)] += total;
    }
    if (blip.isStructure && !blip.isExtractor) {
        record[static_cast<std::size_t>(ThreatSlot::StructuresNotMex)] += total;
    }
    // Components: each blueprint level into its Anti* slot, and the economy level into
    // Structures — the only structure-flavoured component slot the record has.
    record[static_cast<std::size_t>(ThreatSlot::AntiSub)] += blip.sub;
    record[static_cast<std::size_t>(ThreatSlot::AntiAir)] += blip.air;
    record[static_cast<std::size_t>(ThreatSlot::AntiSurface)] += blip.surface;
    record[static_cast<std::size_t>(ThreatSlot::Structures)] += blip.economy;
}

Mag ThreatGrid::cellThreat(const Cell& cell, ThreatSlot type, int army) const noexcept {
    const auto index = static_cast<std::size_t>(type);
    if (army >= 0) {
        // Retail's `army>=0` case indexes the per-army record only — the unassigned
        // bucket belongs to no army (`0x71c600` jump table).
        const auto found = cell.byArmy.find(army);
        return found == cell.byArmy.end() ? Mag{} : found->second[index];
    }
    Mag total = cell.unassigned[index];
    for (const auto& [owner, record] : cell.byArmy) {
        total += record[index];
    }
    return total;
}

Mag ThreatGrid::threatAt(Fx x, Fx z, int rings, ThreatSlot type, int army) const noexcept {
    if (!active()) {
        return Mag{};
    }
    const int gx = x.floorToInt() / cellElmos_;
    const int gz = z.floorToInt() / cellElmos_;
    Mag total{};
    for (int dz = -rings; dz <= rings; ++dz) {
        for (int dx = -rings; dx <= rings; ++dx) {
            const int cx = gx + dx;
            const int cz = gz + dz;
            if (cx < 0 || cz < 0 || cx >= cellsX_ || cz >= cellsZ_) {
                continue;
            }
            total += cellThreat(cells_[static_cast<std::size_t>(cz * cellsX_ + cx)],
                                type, army);
        }
    }
    return total;
}

void ThreatGrid::threatsAround(Fx x, Fx z, Fx radius, ThreatSlot type, int army,
                               std::vector<ThreatRow>& out) const {
    if (!active()) {
        return;
    }
    // Cells whose CENTRE lies inside the radius — the walker `0x71ca70` iterates cells,
    // so the answer is per-cell, not per-contact.
    const FxWide radiusSq = FxWide{radius.raw()} * FxWide{radius.raw()};
    for (int gz = 0; gz < cellsZ_; ++gz) {
        for (int gx = 0; gx < cellsX_; ++gx) {
            const std::int32_t cell = gz * cellsX_ + gx;
            const auto [cx, cz] = cellCentre(cell);
            const FxWide dx = FxWide{cx.raw()} - FxWide{x.raw()};
            const FxWide dz = FxWide{cz.raw()} - FxWide{z.raw()};
            if (dx * dx + dz * dz > radiusSq) {
                continue;
            }
            const Mag threat = cellThreat(cells_[static_cast<std::size_t>(cell)], type, army);
            if (threat > Mag{}) {
                out.push_back({.x = cx, .z = cz, .threat = threat});
            }
        }
    }
}

Mag ThreatGrid::threatBetween(Fx ax, Fx az, Fx bx, Fx bz, ThreatSlot type,
                              int army) const noexcept {
    if (!active()) {
        return Mag{};
    }
    // Walk the segment at half-cell steps so no crossed cell is skipped, summing each
    // cell once — the scalar form of the region walker's `addss` accumulation. The step
    // count comes from the dominant axis (Chebyshev): a square cell is crossed whenever
    // EITHER coordinate moves a cell, so sampling every half-cell of the larger span
    // visits every cell the segment touches without needing a square root.
    const Fx dx = bx - ax;
    const Fx dz = bz - az;
    const FxWide spanRaw = std::max<FxWide>(dx.raw() < 0 ? -FxWide{dx.raw()} : dx.raw(),
                                          dz.raw() < 0 ? -FxWide{dz.raw()} : dz.raw());
    const int steps = std::max<int>(1, static_cast<int>(
        spanRaw * 2 / (FxWide{cellElmos_} << kFxFractionalBits)) + 1);
    Mag total{};
    std::int32_t last = kNoCell;
    for (int i = 0; i <= steps; ++i) {
        // Fixed-point lerp: t = i/steps applied to the raw span.
        const Fx px = Fx::fromRaw(saturate(FxWide{ax.raw()} + FxWide{dx.raw()} * i / steps));
        const Fx pz = Fx::fromRaw(saturate(FxWide{az.raw()} + FxWide{dz.raw()} * i / steps));
        const std::int32_t cell = cellAt(px, pz);
        if (cell == kNoCell || cell == last) {
            continue;
        }
        last = cell;
        total += cellThreat(cells_[static_cast<std::size_t>(cell)], type, army);
    }
    return total;
}

std::optional<std::pair<std::array<Fx, 2>, Mag>> ThreatGrid::highestThreat(
    int rings, ThreatSlot type, int army) const noexcept {
    if (!active()) {
        return std::nullopt;
    }
    // `GetHighestThreatPosition` (`0x71db2a`/`0x71dbca`): comiss max-tracking over the
    // same rings-neighbourhood sums `threatAt` computes.
    Mag best{};
    std::int32_t bestCell = kNoCell;
    for (int gz = 0; gz < cellsZ_; ++gz) {
        for (int gx = 0; gx < cellsX_; ++gx) {
            Mag sum{};
            for (int dz = -rings; dz <= rings; ++dz) {
                for (int dx = -rings; dx <= rings; ++dx) {
                    const int cx = gx + dx;
                    const int cz = gz + dz;
                    if (cx < 0 || cz < 0 || cx >= cellsX_ || cz >= cellsZ_) {
                        continue;
                    }
                    sum += cellThreat(cells_[static_cast<std::size_t>(cz * cellsX_ + cx)],
                                      type, army);
                }
            }
            if (sum > best) {
                best = sum;
                bestCell = gz * cellsX_ + gx;
            }
        }
    }
    if (bestCell == kNoCell) {
        return std::nullopt;
    }
    return std::pair{cellCentre(bestCell), best};
}

} // namespace rm::sim
