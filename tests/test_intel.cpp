// The intel grid: who can see which square, and how removal undoes exactly what
// addition did (ADR-037).

#include "core/sim/Intel.hpp"

#include "core/map/HeightField.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Terrain.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using rm::sim::Fx;
using rm::sim::IntelGrid;

namespace {

/// A square index from grid coordinates, for tests that want to name one.
[[nodiscard]] std::int32_t squareOf(const IntelGrid& grid, int x, int z) {
    return z * grid.squaresX() + x;
}

/// The world centre of a square, which is where a coverage test is answered.
[[nodiscard]] Fx centreOf(const IntelGrid& grid, int square) {
    const int x = square % grid.squaresX();
    return Fx::fromInt(x) * grid.squareElmos() + grid.squareElmos() / Fx::fromInt(2);
}

} // namespace

TEST_CASE("a grid covers the map at its own resolution") {
    // 512 elmos across at mip 0 is 8 elmos a square — SQUARE_SIZE, the resolution
    // Recoil's own heightmap is stated in.
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(256), 0};

    CHECK(grid.squaresX() == 64);
    CHECK(grid.squaresZ() == 32);
    CHECK(grid.squareElmos() == Fx::fromInt(8));

    // Each mip level halves it, exactly as `mipDiv = SQUARE_SIZE * (1 << mipLevel)`.
    const IntelGrid coarse{Fx::fromInt(512), Fx::fromInt(256), 2};
    CHECK(coarse.squaresX() == 16);
    CHECK(coarse.squareElmos() == Fx::fromInt(32));
}

TEST_CASE("a position off the map has no square rather than the nearest one") {
    // Clamping would put a unit standing past the border into the border square and
    // give it sight there. Off the map is off the grid.
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(256), 0};

    CHECK(grid.squareAt(Fx::fromInt(0), Fx::fromInt(0)) == 0);
    CHECK(grid.squareAt(Fx::fromInt(-1), Fx::fromInt(0)) == IntelGrid::kNoSquare);
    CHECK(grid.squareAt(Fx::fromInt(0), Fx::fromInt(-1)) == IntelGrid::kNoSquare);
    CHECK(grid.squareAt(Fx::fromInt(512), Fx::fromInt(0)) == IntelGrid::kNoSquare);
    CHECK(grid.squareAt(Fx::fromInt(0), Fx::fromInt(256)) == IntelGrid::kNoSquare);
}

TEST_CASE("a square is covered while something counts it and not after") {
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(256), 0};
    const std::int32_t square = squareOf(grid, 10, 5);
    const std::vector<std::int32_t> shape{square};

    CHECK_FALSE(grid.covered(square));

    grid.add(shape);
    CHECK(grid.covered(square));
    CHECK(grid.count(square) == 1);

    grid.remove(shape);
    CHECK_FALSE(grid.covered(square));
    CHECK(grid.count(square) == 0);
}

TEST_CASE("two emitters over one square keep it lit until BOTH leave") {
    // The reason the grid holds a count and not a flag, and the reason it is worth a
    // test of its own: with a boolean, the first unit to walk away takes the second
    // unit's sight with it, and the symptom — vision flickering off where two units
    // overlap — looks like anything but a type error.
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(256), 0};
    const std::vector<std::int32_t> shape{squareOf(grid, 3, 3)};

    grid.add(shape);
    grid.add(shape);
    CHECK(grid.count(shape[0]) == 2);

    grid.remove(shape);
    CHECK(grid.covered(shape[0]));

    grid.remove(shape);
    CHECK_FALSE(grid.covered(shape[0]));
}

TEST_CASE("a circle of no radius still covers the square it stands on") {
    // 8 of the 568 blueprints state a vision radius under one square. Rounding them
    // to nothing would make a unit unable to see its own feet, which is a worse
    // reading of "sees a little" than "sees one square".
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};
    std::vector<std::int32_t> shape;

    rm::sim::circleSquares(grid, Fx::fromInt(100), Fx::fromInt(100), Fx::fromInt(1), shape);

    REQUIRE(shape.size() == 1);
    CHECK(shape[0] == grid.squareAt(Fx::fromInt(100), Fx::fromInt(100)));
}

TEST_CASE("a circle covers what is inside it and nothing outside") {
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};
    std::vector<std::int32_t> shape;

    // Radius 80 elmos = 10 squares, about a 316-square disc.
    const Fx x = Fx::fromInt(256);
    const Fx z = Fx::fromInt(256);
    rm::sim::circleSquares(grid, x, z, Fx::fromInt(80), shape);
    grid.add(shape);

    // Dead centre and just inside the rim, both lit.
    CHECK(grid.covered(grid.squareAt(x, z)));
    CHECK(grid.covered(grid.squareAt(x + Fx::fromInt(70), z)));

    // Well outside, dark — including the corner of the bounding box, which is what
    // separates a disc from a square.
    CHECK_FALSE(grid.covered(grid.squareAt(x + Fx::fromInt(100), z)));
    CHECK_FALSE(grid.covered(grid.squareAt(x + Fx::fromInt(72), z + Fx::fromInt(72))));

    // No square is counted twice. The midpoint algorithm emits the disc row by row and
    // a duplicated row would double every count in it — harmless for `covered`, since
    // removal doubles too, and wrong for everything that reads the number.
    std::vector<std::int32_t> sorted = shape;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());

    // And it is a disc, not a box: 349 squares for a radius of 10.
    //
    // That is pi*(r + 1/2)^2 = 346, NOT pi*r^2 = 314, and the half is real rather than
    // slop — the rasteriser fills up to and including the boundary square on each side,
    // so the disc it draws is half a square wider all round than the radius it was asked
    // for. Recoil's is the same shape. A box of the same radius would be 441.
    CHECK(shape.size() == 349);
}

TEST_CASE("a circle at the map edge is clipped, not wrapped") {
    // Wrapping would light the far side of the map. Worth its own test because the
    // index arithmetic makes it the natural bug: a row that runs past the right edge
    // continues into the start of the next one.
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};
    std::vector<std::int32_t> shape;

    rm::sim::circleSquares(grid, Fx::fromInt(4), Fx::fromInt(256), Fx::fromInt(80), shape);
    grid.add(shape);

    // Nothing on the right-hand edge of any row.
    for (int z = 0; z < grid.squaresZ(); ++z) {
        CHECK(grid.count(squareOf(grid, grid.squaresX() - 1, z)) == 0);
    }
    // Every covered square really is within the radius of the emitter.
    for (const std::int32_t square : shape) {
        CHECK(centreOf(grid, square) < Fx::fromInt(4 + 80 + 8));
    }
}

TEST_CASE("adding a shape and removing it leaves the grid exactly as it was") {
    // The property the whole design rests on: withdrawal is exact, so a unit that
    // moves leaves no residue and a unit that dies takes only its own sight with it.
    IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 1};
    std::vector<std::int32_t> a;
    std::vector<std::int32_t> b;

    rm::sim::circleSquares(grid, Fx::fromInt(100), Fx::fromInt(100), Fx::fromInt(64), a);
    rm::sim::circleSquares(grid, Fx::fromInt(130), Fx::fromInt(110), Fx::fromInt(48), b);

    grid.add(a);
    const std::vector<std::uint16_t> before{grid.counts().begin(), grid.counts().end()};

    grid.add(b);
    grid.remove(b);

    const std::vector<std::uint16_t> after{grid.counts().begin(), grid.counts().end()};
    CHECK(before == after);
}

// --- The raycast, and the setting that decides whether it runs at all -------------
//
// ADR-037's fork: Recoil raycasts sight and radar against the ground, Supreme Commander
// stamps flat discs and lets you see over mountains. Both are here and the caller picks.

namespace {

/// A flat field of `squares` squares, every corner at `height` elmos.
[[nodiscard]] rm::HeightField flatField(int squares, float height) {
    rm::HeightField field;
    field.squaresX = squares;
    field.squaresZ = squares;
    field.baseHeight = height;
    field.heightScale = 1.0f;  // one elmo per raw step
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// Raises a band of heightmap corners to `height` elmos above the base.
void raiseBand(rm::HeightField& field, int x0, int x1, std::uint16_t raw) {
    for (int z = 0; z < field.verticesZ(); ++z) {
        for (int x = x0; x <= x1 && x < field.verticesX(); ++x) {
            field.raw[static_cast<std::size_t>(z)
                          * static_cast<std::size_t>(field.verticesX())
                      + static_cast<std::size_t>(x)] = raw;
        }
    }
}

} // namespace

TEST_CASE("on flat ground the raycast sees exactly what the disc covers") {
    // The equivalence that makes the setting safe to flip: with nothing in the way, the
    // expensive algorithm and the cheap one agree square for square. Anything else would
    // mean the raycast was losing squares to its own arithmetic rather than to terrain.
    const rm::HeightField field = flatField(64, 50.0f);
    const rm::sim::Terrain terrain{field};
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};

    std::vector<std::int32_t> disc;
    std::vector<std::int32_t> cast;
    const Fx x = Fx::fromInt(256);
    const Fx z = Fx::fromInt(256);
    const Fx radius = Fx::fromInt(80);

    rm::sim::circleSquares(grid, x, z, radius, disc);
    rm::sim::raycastSquares(grid, terrain, x, z, radius, Fx::fromInt(52), cast);

    std::ranges::sort(disc);
    std::ranges::sort(cast);
    CHECK(disc == cast);
}

TEST_CASE("a ridge hides the ground behind it, and only under the Recoil style") {
    // The whole reason the setting exists. Same map, same emitter, same radius; one
    // engine's answer is that the far slope is dark and the other's is that you can see
    // over a mountain, because `vision.fx` never samples a height.
    rm::HeightField field = flatField(64, 0.0f);
    // A wall 200 elmos tall, four squares wide, at x = 300..332 elmos.
    raiseBand(field, 38, 41, 200);
    const rm::sim::Terrain terrain{field};
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};

    const Fx x = Fx::fromInt(200);
    const Fx z = Fx::fromInt(256);
    const Fx radius = Fx::fromInt(200);
    const Fx eye = Fx::fromInt(10);

    std::vector<std::int32_t> cast;
    std::vector<std::int32_t> disc;
    rm::sim::raycastSquares(grid, terrain, x, z, radius, eye, cast);
    rm::sim::circleSquares(grid, x, z, radius, disc);

    // Directly beyond the ridge, on the same row.
    const std::int32_t behind = grid.squareAt(Fx::fromInt(380), z);
    const std::int32_t infront = grid.squareAt(Fx::fromInt(260), z);

    CHECK(std::ranges::find(disc, behind) != disc.end());     // the disc reaches it
    CHECK(std::ranges::find(cast, behind) == cast.end());     // the raycast does not
    CHECK(std::ranges::find(cast, infront) != cast.end());    // the near side is lit

    // Occlusion only ever REMOVES squares. A raycast that lit something the disc did not
    // reach would mean the ray walk had escaped its own radius.
    for (const std::int32_t square : cast) {
        CHECK(std::ranges::find(disc, square) != disc.end());
    }
    CHECK(cast.size() < disc.size());
}

TEST_CASE("raising the eye is what opens a blocked ray") {
    // Height is the point of the algorithm, not a side effect — it is what makes high
    // ground worth taking. Same emitter, same spot, same radius; only the eye moves.
    //
    // A LOW ridge, deliberately. The first draft of this test put the emitter on top of
    // the 200-elmo wall above and expected it to see the ground behind, which is wrong
    // geometry rather than a wrong engine: from ten elmos above a plateau, the plateau's
    // own far edge hides everything below it out to 688 elmos, well off this map. A
    // 20-elmo rise is the case where the eye height decides the answer.
    rm::HeightField field = flatField(64, 0.0f);
    raiseBand(field, 38, 41, 20);
    const rm::sim::Terrain terrain{field};
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};

    const Fx x = Fx::fromInt(200);
    const Fx z = Fx::fromInt(256);
    const Fx radius = Fx::fromInt(200);
    const std::int32_t behind = grid.squareAt(Fx::fromInt(380), z);

    std::vector<std::int32_t> low;
    std::vector<std::int32_t> high;
    rm::sim::raycastSquares(grid, terrain, x, z, radius, Fx::fromInt(10), low);
    rm::sim::raycastSquares(grid, terrain, x, z, radius, Fx::fromInt(100), high);

    CHECK(std::ranges::find(low, behind) == low.end());
    CHECK(std::ranges::find(high, behind) != high.end());

    // And it is monotonic: a higher eye never LOSES a square, it only gains them.
    CHECK(high.size() > low.size());
    for (const std::int32_t square : low) {
        CHECK(std::ranges::find(high, square) != high.end());
    }
}

TEST_CASE("the style decides which senses are raycast") {
    // Recoil raycasts sight and radar and leaves sonar a disc (LosHandler.cpp:92); FA
    // stamps discs for everything. This is the dispatch that says so.
    rm::HeightField field = flatField(64, 0.0f);
    raiseBand(field, 38, 41, 200);
    const rm::sim::Terrain terrain{field};
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};

    const Fx x = Fx::fromInt(200);
    const Fx z = Fx::fromInt(256);
    const Fx radius = Fx::fromInt(200);
    const Fx eye = Fx::fromInt(10);
    const std::int32_t behind = grid.squareAt(Fx::fromInt(380), z);

    std::vector<std::int32_t> squares;
    const auto reaches = [&](rm::sim::VisionStyle style, rm::sim::IntelKind kind) {
        rm::sim::intelSquares(grid, &terrain, style, kind, x, z, radius, eye, squares);
        return std::ranges::find(squares, behind) != squares.end();
    };

    using rm::sim::IntelKind;
    using rm::sim::VisionStyle;

    CHECK(reaches(VisionStyle::ForgedAlliance, IntelKind::Vision));
    CHECK(reaches(VisionStyle::ForgedAlliance, IntelKind::Radar));
    CHECK(reaches(VisionStyle::ForgedAlliance, IntelKind::Sonar));

    CHECK_FALSE(reaches(VisionStyle::Recoil, IntelKind::Vision));
    CHECK_FALSE(reaches(VisionStyle::Recoil, IntelKind::Radar));
    CHECK(reaches(VisionStyle::Recoil, IntelKind::Sonar));
}

TEST_CASE("with no terrain to consult, the Recoil style falls back to discs") {
    // A `--units` crowd on procedural ground has no sim terrain, and the honest answer
    // there is the disc rather than a raycast against a heightmap that is not there.
    const IntelGrid grid{Fx::fromInt(512), Fx::fromInt(512), 0};
    std::vector<std::int32_t> squares;
    std::vector<std::int32_t> disc;

    rm::sim::intelSquares(grid, nullptr, rm::sim::VisionStyle::Recoil,
                          rm::sim::IntelKind::Vision, Fx::fromInt(256), Fx::fromInt(256),
                          Fx::fromInt(80), Fx::fromInt(50), squares);
    rm::sim::circleSquares(grid, Fx::fromInt(256), Fx::fromInt(256), Fx::fromInt(80), disc);

    CHECK(squares == disc);
}

// --- The pass: every unit's coverage, kept current as the match moves ---------------

namespace {

using rm::sim::Army;
using rm::sim::Intel;
using rm::sim::IntelKind;
using rm::sim::UnitCatalog;
using rm::sim::UnitStore;

/// A definition that sees `vision` elmos and nothing else, owned by the caller.
[[nodiscard]] rm::unitdef::UnitDef seer(float vision, float radar = 0.0f) {
    rm::unitdef::UnitDef def;
    def.visionRadiusElmos = vision;
    def.radarRadiusElmos = radar;
    return def;
}

/// Two armies, allied or not.
[[nodiscard]] std::vector<Army> twoArmies(bool allied) {
    std::vector<Army> armies(2);
    armies[0].index = 0;
    armies[0].alliance = 0;
    armies[1].index = 1;
    armies[1].alliance = allied ? 0 : 1;
    return armies;
}

/// Puts a unit of `type` belonging to `army` at (x, z).
rm::sim::UnitId place(UnitStore& store, rm::UnitTypeIndex type, int army, float x, float z) {
    UnitStore::Spawn spawn;
    spawn.type = type;
    spawn.transform.x = rm::sim::fxFromFloat(x);
    spawn.transform.z = rm::sim::fxFromFloat(z);
    spawn.motion.armyIndex = army;
    spawn.health.current = rm::sim::magFromFloat(100.0f);
    spawn.health.maximum = spawn.health.current;
    return store.spawn(spawn);
}

} // namespace

TEST_CASE("a unit lights the ground around it for its own alliance only") {
    const rm::unitdef::UnitDef def = seer(80.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, type, 0, 256.0f, 256.0f);
    std::vector<Army> armies = twoArmies(false);

    intel.update(store, catalog, armies, nullptr);

    const Fx here = Fx::fromInt(256);
    CHECK(intel.sees(0, IntelKind::Vision, here, here));
    CHECK_FALSE(intel.sees(1, IntelKind::Vision, here, here));

    // A sense the unit does not carry stays dark even where it stands.
    CHECK_FALSE(intel.sees(0, IntelKind::Radar, here, here));
}

TEST_CASE("allies share what either of them can see") {
    // The reason the grids are keyed on alliance rather than army, checked rather than
    // asserted in a comment.
    const rm::unitdef::UnitDef def = seer(80.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, type, 1, 100.0f, 100.0f);  // army 1, allied with army 0
    std::vector<Army> armies = twoArmies(true);

    intel.update(store, catalog, armies, nullptr);

    CHECK(intel.sees(0, IntelKind::Vision, Fx::fromInt(100), Fx::fromInt(100)));
}

TEST_CASE("a unit that moves takes its sight with it, leaving nothing behind") {
    const rm::unitdef::UnitDef def = seer(80.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(1, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    const rm::sim::UnitId unit = place(store, type, 0, 100.0f, 100.0f);
    std::vector<Army> armies = twoArmies(false);
    armies[1].alliance = 0;

    intel.update(store, catalog, armies, nullptr);
    CHECK(intel.sees(0, IntelKind::Vision, Fx::fromInt(100), Fx::fromInt(100)));

    store.transforms()[unit.index].x = Fx::fromInt(400);
    store.transforms()[unit.index].z = Fx::fromInt(400);
    intel.update(store, catalog, armies, nullptr);

    CHECK(intel.sees(0, IntelKind::Vision, Fx::fromInt(400), Fx::fromInt(400)));
    CHECK_FALSE(intel.sees(0, IntelKind::Vision, Fx::fromInt(100), Fx::fromInt(100)));

    // NO RESIDUE ANYWHERE. Not just "the old spot is dark" — the whole grid holds exactly
    // one disc's worth of counts, which is what says the withdrawal was exact rather than
    // approximately right.
    std::size_t lit = 0;
    for (const std::uint16_t count : intel.grid(0, IntelKind::Vision).counts()) {
        CHECK(count <= 1);
        lit += count;
    }
    std::vector<std::int32_t> disc;
    rm::sim::circleSquares(intel.grid(0, IntelKind::Vision), Fx::fromInt(400),
                           Fx::fromInt(400), Fx::fromInt(80), disc);
    CHECK(lit == disc.size());
}

TEST_CASE("a unit that dies stops seeing") {
    const rm::unitdef::UnitDef def = seer(80.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(1, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    const rm::sim::UnitId unit = place(store, type, 0, 100.0f, 100.0f);
    std::vector<Army> armies = twoArmies(true);

    intel.update(store, catalog, armies, nullptr);
    REQUIRE(intel.sees(0, IntelKind::Vision, Fx::fromInt(100), Fx::fromInt(100)));

    store.kill(unit);
    intel.update(store, catalog, armies, nullptr);
    CHECK_FALSE(intel.sees(0, IntelKind::Vision, Fx::fromInt(100), Fx::fromInt(100)));
}

TEST_CASE("two units overlapping, one leaves, the ground between them stays lit") {
    // The refcount property, now through the pass rather than the grid directly — this is
    // the case a boolean grid gets wrong in a way nobody would think to look for.
    const rm::unitdef::UnitDef def = seer(80.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(1, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    const rm::sim::UnitId first = place(store, type, 0, 200.0f, 200.0f);
    (void)place(store, type, 0, 230.0f, 200.0f);
    std::vector<Army> armies = twoArmies(true);

    intel.update(store, catalog, armies, nullptr);
    const Fx betweenX = Fx::fromInt(215);
    const Fx betweenZ = Fx::fromInt(200);
    REQUIRE(intel.sees(0, IntelKind::Vision, betweenX, betweenZ));

    store.kill(first);
    intel.update(store, catalog, armies, nullptr);
    CHECK(intel.sees(0, IntelKind::Vision, betweenX, betweenZ));
}

TEST_CASE("intel that was never configured answers seen, not blind") {
    // Every scene predating this file — a `--units` crowd, most tests — runs with no intel
    // at all, and the answer there has to be the engine those scenes were written against.
    const Intel intel;
    CHECK(intel.sees(0, IntelKind::Vision, Fx::fromInt(1), Fx::fromInt(1)));
    CHECK_FALSE(intel.active());
}

TEST_CASE("the same units in the same places produce the same grid twice over") {
    // Determinism, at the level this pass can be checked at on one machine: the same
    // inputs stamped twice give byte-identical counts. `--hash-log` is what checks it
    // across runs, and this is what would fail first if the pass depended on iteration
    // order or on a stale scratch buffer.
    const rm::unitdef::UnitDef def = seer(120.0f, 300.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);
    std::vector<Army> armies = twoArmies(false);

    const auto build = [&] {
        Intel intel;
        intel.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                        rm::sim::VisionStyle::ForgedAlliance);
        UnitStore store;
        (void)place(store, type, 0, 120.0f, 300.0f);
        (void)place(store, type, 1, 400.0f, 110.0f);
        (void)place(store, type, 0, 260.0f, 260.0f);
        intel.update(store, catalog, armies, nullptr);

        std::vector<std::uint16_t> flat;
        for (int alliance = 0; alliance < 2; ++alliance) {
            for (const IntelKind kind : {IntelKind::Vision, IntelKind::Radar, IntelKind::Sonar}) {
                const auto counts = intel.grid(alliance, kind).counts();
                flat.insert(flat.end(), counts.begin(), counts.end());
            }
        }
        return flat;
    };

    CHECK(build() == build());
}
