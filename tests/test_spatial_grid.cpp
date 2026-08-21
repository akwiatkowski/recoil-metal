// Which units are near a place.
//
// THE ORACLE IS THE POINT. A spatial index's whole job is to give the answer a full scan would
// give, faster — so the full scan is the specification, and a randomised comparison against it
// is a stronger test than any number of hand-written cases could be. §7 P5.1 asks for 10,000
// randomised layouts; this does that and then adds the cases the random generator is unlikely
// to produce on its own (everything in one cell, negative coordinates, a radius that spans the
// whole map).
#include <catch2/catch_test_macros.hpp>

#include "core/sim/SpatialGrid.hpp"

#include "support/TestRoster.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using rm::sim::Fx;
using rm::sim::SpatialGrid;
using rm::UnitIndex;

namespace {

/// A reproducible pseudo-random source.
///
/// Its own generator rather than `<random>`: the standard distributions are not specified to
/// produce the same sequence across implementations, and a randomised test that cannot be
/// replayed on another machine is a test that reports a failure nobody can reproduce. xorshift64
/// is four lines and is the same everywhere.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_{seed} {}

    [[nodiscard]] std::uint64_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }

    /// An integer in [low, high].
    [[nodiscard]] std::int32_t between(std::int32_t low, std::int32_t high) noexcept {
        const auto span = static_cast<std::uint64_t>(high - low + 1);
        return low + static_cast<std::int32_t>(next() % span);
    }

private:
    std::uint64_t state_ = 1;
};

/// THE ORACLE: every slot within `radius` of (x, z) on the ground, by a full scan, in slot
/// order. Deliberately the dumbest possible implementation — it is the specification, so being
/// obviously right matters more than being quick.
[[nodiscard]] std::vector<UnitIndex> bruteForce(const rm::sim::UnitStore& store, Fx x, Fx z,
                                                Fx radius) {
    std::vector<UnitIndex> found;
    const std::span<const rm::sim::Transform> transforms = store.transforms();
    for (UnitIndex slot = 0; slot < transforms.size(); ++slot) {
        const Fx dx = transforms[slot].x - x;
        const Fx dz = transforms[slot].z - z;
        if (rm::sim::fxHypot(dx, dz) <= radius) {
            found.push_back(slot);
        }
    }
    return found;
}

/// How many cells a query would have to walk — the same arithmetic `gather` uses to decide
/// between the cell path and the whole-array scan.
///
/// DUPLICATED IN THE TEST ON PURPOSE. The grid's two paths are meant to be indistinguishable in
/// their answers, which is exactly what makes it possible to test only one of them by accident:
/// the first version of this file did, and a deliberately broken cell key passed it. Computing
/// the branch here lets the test assert that both paths were actually exercised.
[[nodiscard]] std::int64_t cellsWalked(Fx x, Fx z, Fx radius, Fx cellSize) {
    const std::int64_t x0 = ((x - radius) / cellSize).floorToInt();
    const std::int64_t x1 = ((x + radius) / cellSize).floorToInt();
    const std::int64_t z0 = ((z - radius) / cellSize).floorToInt();
    const std::int64_t z1 = ((z + radius) / cellSize).floorToInt();
    return (x1 - x0 + 1) * (z1 - z0 + 1);
}

/// A unit at a place, with nothing else about it that matters here.
void place(rm::test::Roster& roster, rm::UnitTypeIndex type, float x, float z) {
    (void)roster.add(type, x, z, 0, 100.0f);
}

} // namespace

TEST_CASE("a query agrees with brute force over ten thousand randomised layouts") {
    // §7 P5.1's stated test, and the reason it is the ideal shape: the slow version is the
    // specification, so a randomised comparison cannot be wrong in the same direction as the
    // code.
    //
    // THE PARAMETERS ARE CHOSEN TO EXERCISE BOTH QUERY PATHS, and that took a correction.
    // The first version used up to 24 units with radii up to 500, so the cell range almost
    // always exceeded the unit count and the whole-array scan answered nearly every probe —
    // which meant the cell path, the binary search and the packed cell key were all
    // effectively untested. Removing the sign bias from the key (a real bug, and the one the
    // negative-coordinate case below exists for) left every assertion green. So: layouts up to
    // 200 units, radii weighted small, and an assertion at the end that each path ran.
    Rng rng{0x5EED'1234'ABCD'0001ull};

    std::size_t agreed = 0;
    std::size_t nonEmpty = 0;
    std::size_t viaCells = 0;
    std::size_t viaScan = 0;

    for (int layout = 0; layout < 10'000; ++layout) {
        rm::test::Roster roster;
        rm::unitdef::UnitDef def;
        def.name = "test_thing";
        const rm::UnitTypeIndex type = roster.addType(def);

        const std::int32_t count = rng.between(0, 200);
        for (std::int32_t i = 0; i < count; ++i) {
            // Snapped to a multiple of 8 a third of the time, so cell boundaries — where an
            // off-by-one in the cell arithmetic lives — are hit constantly rather than never.
            // The range straddles zero, so negative cell coordinates are ordinary here.
            const std::int32_t rawX = rng.between(-200, 600);
            const std::int32_t rawZ = rng.between(-200, 600);
            const bool snap = rng.between(0, 2) == 0;
            place(roster, type, static_cast<float>(snap ? rawX - rawX % 8 : rawX),
                  static_cast<float>(snap ? rawZ - rawZ % 8 : rawZ));
        }

        const Fx cellSize = Fx::fromInt(rng.between(1, 96));
        SpatialGrid grid;
        grid.rebuild(roster.store, cellSize);
        REQUIRE(grid.size() == roster.store.slotCount());

        for (int probe = 0; probe < 4; ++probe) {
            const Fx x = Fx::fromInt(rng.between(-300, 700));
            const Fx z = Fx::fromInt(rng.between(-300, 700));
            // Weighted small: a quarter of the probes are wide, the rest are the sort of radius
            // a real weapon or blast has, which is what keeps the cell path in play.
            const Fx radius = Fx::fromInt(rng.between(0, 2) == 0 ? rng.between(0, 500)
                                                                 : rng.between(0, 60));

            const std::vector<UnitIndex> expected = bruteForce(roster.store, x, z, radius);
            const std::span<const UnitIndex> actual = grid.within(x, z, radius);

            // SEQUENCE equality, not set equality. Ascending slot order is part of the grid's
            // contract, because every caller's tie-break depends on meeting the lowest slot
            // first — so a result that held the right units in the wrong order would be a
            // silent behaviour change rather than a passing test.
            const bool same = std::equal(expected.begin(), expected.end(), actual.begin(),
                                         actual.end());
            if (!same) {
                // Reported once, with everything needed to reproduce it, rather than as 40,000
                // CHECK lines. A failure here is a specific layout and a specific probe.
                FAIL("layout " << layout << " probe " << probe << ": expected "
                               << expected.size() << " slots, got " << actual.size()
                               << " (cell " << cellSize.raw() << ", radius " << radius.raw()
                               << ")");
            }
            ++agreed;
            if (!expected.empty()) {
                ++nonEmpty;
            }
            if (cellsWalked(x, z, radius, grid.cellSize())
                >= static_cast<std::int64_t>(grid.size())) {
                ++viaScan;
            } else {
                ++viaCells;
            }
        }
    }

    CHECK(agreed == 40'000);
    // Not vacuous: a run where every probe found nothing would agree with brute force
    // perfectly and prove nothing at all. The measured share is about 42% — lower than it was
    // when radii ran up to 500 elmos, which is the price of weighting them small enough to keep
    // the cell path in play, and 17,000 probes that found something is ample.
    CHECK(nonEmpty > 10'000);
    // And not half-tested: both query paths carried a real share of the probes.
    CHECK(viaCells > 5'000);
    CHECK(viaScan > 5'000);
}

TEST_CASE("an empty grid answers nothing rather than crashing") {
    rm::test::Roster roster;
    SpatialGrid grid;
    grid.rebuild(roster.store, Fx::fromInt(32));

    CHECK(grid.size() == 0);
    CHECK(grid.within(Fx{}, Fx{}, Fx::fromInt(100)).empty());
    CHECK(grid.candidates(Fx{}, Fx{}, Fx::fromInt(100)).empty());
}

TEST_CASE("a cell size of zero is floored rather than dividing by it") {
    // A scene of markers has no collision radius at all, so a caller deriving its cell size
    // from "twice the largest radius" asks for zero. That must be a floor, not a crash.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    const rm::UnitTypeIndex type = roster.addType(def);
    place(roster, type, 10.0f, 10.0f);

    SpatialGrid grid;
    grid.rebuild(roster.store, Fx{});
    CHECK(grid.cellSize() == SpatialGrid::kMinCellSize);
    CHECK(grid.within(Fx::fromInt(10), Fx::fromInt(10), Fx::fromInt(1)).size() == 1);
}

TEST_CASE("a query straddling the origin finds units on both sides of it") {
    // Negative cell coordinates, on the cell path. The key packs two signed coordinates into one
    // unsigned integer, so a cell at x = -1 packs to a very large key and sorts above x = 0 —
    // which is FINE, and this case is what established that: the array is sorted by the key and
    // every lookup searches by the same key, so any total order groups a cell's entries
    // contiguously. An earlier version biased each half to make the order spatial, and deleting
    // the bias left this green, which is why the bias is gone.
    //
    // ENOUGH UNITS TO FORCE THE CELL PATH. With a handful of units the query falls through to
    // the whole-array scan, where the entry order is irrelevant — so a case meant to exercise
    // the binary search has to put the query on the cell side of the threshold, and assert that
    // it did.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    const rm::UnitTypeIndex type = roster.addType(def);
    for (int i = 0; i < 100; ++i) {
        const auto spread = static_cast<float>(i * 13);
        place(roster, type, -400.0f - spread, -400.0f - spread);
        place(roster, type, 400.0f + spread, 400.0f + spread);
        place(roster, type, -400.0f - spread, 400.0f + spread);
        place(roster, type, 400.0f + spread, -400.0f - spread);
    }
    // The two that straddle the origin, on either side of it.
    const auto left = static_cast<UnitIndex>(roster.store.slotCount());
    place(roster, type, -8.0f, -8.0f);
    place(roster, type, 8.0f, 8.0f);

    SpatialGrid grid;
    grid.rebuild(roster.store, Fx::fromInt(16));
    REQUIRE(cellsWalked(Fx{}, Fx{}, Fx::fromInt(20), grid.cellSize())
            < static_cast<std::int64_t>(grid.size()));  // the cell path, not the scan

    const std::span<const UnitIndex> near = grid.within(Fx{}, Fx{}, Fx::fromInt(20));
    REQUIRE(near.size() == 2);
    CHECK(near[0] == left);
    CHECK(near[1] == left + 1);
}

TEST_CASE("everything in one cell is still found") {
    // The degenerate case a randomised generator produces rarely: a rally point, where every
    // unit is at the same place and one cell holds the whole match.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    const rm::UnitTypeIndex type = roster.addType(def);
    for (int i = 0; i < 50; ++i) {
        place(roster, type, 100.0f, 100.0f);
    }

    SpatialGrid grid;
    grid.rebuild(roster.store, Fx::fromInt(64));
    CHECK(grid.within(Fx::fromInt(100), Fx::fromInt(100), Fx::fromInt(1)).size() == 50);
    CHECK(grid.within(Fx::fromInt(400), Fx::fromInt(400), Fx::fromInt(1)).empty());
}

TEST_CASE("a radius spanning the whole map takes the scan, not four thousand cells") {
    // The adaptive path. Behaviourally invisible — which is the point — so what is checked is
    // that it gives the same answer for a query whose cell range dwarfs the unit count.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    const rm::UnitTypeIndex type = roster.addType(def);
    for (int i = 0; i < 8; ++i) {
        place(roster, type, static_cast<float>(i * 500), static_cast<float>(i * 500));
    }

    SpatialGrid grid;
    grid.rebuild(roster.store, Fx::fromInt(8));  // 8-elmo cells, a 4,000-elmo query

    const std::vector<UnitIndex> expected =
        bruteForce(roster.store, Fx::fromInt(1750), Fx::fromInt(1750), Fx::fromInt(4000));
    const std::span<const UnitIndex> actual =
        grid.within(Fx::fromInt(1750), Fx::fromInt(1750), Fx::fromInt(4000));
    CHECK(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()));
    CHECK(actual.size() == 8);
}

TEST_CASE("candidates are a superset of within, and share its order") {
    // `candidates` skips the distance test, so it returns whole cells. Two properties matter:
    // it never MISSES anything `within` would return, and it comes back in the same ascending
    // slot order — a caller doing its own distance test relies on both.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    const rm::UnitTypeIndex type = roster.addType(def);
    for (int i = 0; i < 40; ++i) {
        place(roster, type, static_cast<float>((i * 37) % 400),
              static_cast<float>((i * 53) % 400));
    }

    SpatialGrid grid;
    grid.rebuild(roster.store, Fx::fromInt(32));

    const Fx x = Fx::fromInt(200);
    const Fx z = Fx::fromInt(200);
    const Fx radius = Fx::fromInt(60);

    const std::vector<UnitIndex> exact{grid.within(x, z, radius).begin(),
                                       grid.within(x, z, radius).end()};
    const std::span<const UnitIndex> loose = grid.candidates(x, z, radius);

    CHECK(loose.size() >= exact.size());
    CHECK(std::is_sorted(loose.begin(), loose.end()));
    CHECK(std::includes(loose.begin(), loose.end(), exact.begin(), exact.end()));
}

TEST_CASE("a query answered twice gives the same answer") {
    // The result span points into the grid and is invalidated by the next query — a documented
    // bargain, and one worth a test so that "invalidated" does not quietly become "corrupted".
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    const rm::UnitTypeIndex type = roster.addType(def);
    for (int i = 0; i < 20; ++i) {
        place(roster, type, static_cast<float>(i * 11), static_cast<float>(i * 7));
    }

    SpatialGrid grid;
    grid.rebuild(roster.store, Fx::fromInt(24));

    const std::vector<UnitIndex> first{grid.within(Fx::fromInt(50), Fx::fromInt(30),
                                                   Fx::fromInt(40)).begin(),
                                       grid.within(Fx::fromInt(50), Fx::fromInt(30),
                                                   Fx::fromInt(40)).end()};
    (void)grid.within(Fx::fromInt(900), Fx::fromInt(900), Fx::fromInt(10));  // a different query
    const std::span<const UnitIndex> again =
        grid.within(Fx::fromInt(50), Fx::fromInt(30), Fx::fromInt(40));

    CHECK(std::equal(first.begin(), first.end(), again.begin(), again.end()));
}
