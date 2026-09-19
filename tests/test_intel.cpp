// The intel grid: who can see which square, and how removal undoes exactly what
// addition did (ADR-037).

#include "core/sim/Intel.hpp"

#include "core/map/HeightField.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/SaveState.hpp"
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

/// The drift period at the default clock, derived the way the sim derives it.
[[nodiscard]] int blipDriftTicks() {
    return static_cast<int>(rm::sim::TickRate{}.ticks(rm::sim::kBlipDriftPeriod));
}

/// How far a blip may move in one tick. Two full radii over one period is the worst the
/// interpolation can do, so anything near that is drift and anything above it is a jump.
[[nodiscard]] int radarErrorStepBound() {
    return 2 * rm::sim::kRadarErrorElmos / blipDriftTicks() + 2;
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

TEST_CASE("a unit's sensors sit on top of it, not on the ground under it") {
    // THE PLUMBING, not the algorithm. `raycastSquares` has always taken an eye height and the
    // tests above prove it uses one; what nothing checked is what the SIM hands it, and the sim
    // was handing it the unit's transform — the terrain under its feet. Every unit in the game
    // therefore looked out from ankle level, and a commander two ogrids tall could not see over
    // a rise its own head cleared.
    // A LOW ridge, for the reason the eye-height test above records: from high enough above a
    // tall plateau its own FAR edge hides the ground beyond, so a taller eye stops helping. A
    // twenty-elmo rise is the band where the eye decides the answer.
    rm::HeightField field = flatField(64, 0.0f);
    raiseBand(field, 38, 41, 20);  // a 20-elmo ridge at x = 300..332
    const rm::sim::Terrain terrain{field};

    rm::unitdef::UnitDef low = seer(400.0f);
    rm::unitdef::UnitDef tall = seer(400.0f);
    tall.sizeYElmos = 100.0f;  // the same eye the raycast's own test proves opens this ray

    UnitCatalog catalog;
    const rm::UnitTypeIndex lowType = catalog.add(&low);
    const rm::UnitTypeIndex tallType = catalog.add(&tall);
    CHECK(catalog.intel(tallType).eyeHeight > catalog.intel(lowType).eyeHeight);

    const Fx behind = Fx::fromInt(380);  // the far side of the ridge
    const Fx lane = Fx::fromInt(256);

    const auto seesBehindRidge = [&](rm::UnitTypeIndex type) {
        Intel intel;
        intel.configure(2, Fx::fromInt(512), Fx::fromInt(512), rm::sim::VisionStyle::Recoil);
        UnitStore store;
        (void)place(store, type, 0, 200.0f, 256.0f);
        std::vector<Army> armies = twoArmies(false);
        intel.update(store, catalog, armies, &terrain);
        return intel.sees(0, IntelKind::Vision, behind, lane);
    };

    CHECK_FALSE(seesBehindRidge(lowType));
    CHECK(seesBehindRidge(tallType));
}

TEST_CASE("the flat-disc style ignores height entirely, as its own shader does") {
    // The other half of the same decision, and the reason `--vision-style fa` is now the
    // default: Supreme Commander's `vision.fx` has no heightmap sample in it at all. A sensor
    // height that changed a disc would be this engine inventing a rule.
    rm::HeightField field = flatField(64, 0.0f);
    raiseBand(field, 38, 41, 60);
    const rm::sim::Terrain terrain{field};

    rm::unitdef::UnitDef low = seer(400.0f);
    rm::unitdef::UnitDef tall = seer(400.0f);
    tall.sizeYElmos = 200.0f;

    UnitCatalog catalog;
    const rm::UnitTypeIndex lowType = catalog.add(&low);
    const rm::UnitTypeIndex tallType = catalog.add(&tall);

    const auto litSquares = [&](rm::UnitTypeIndex type) {
        Intel intel;
        intel.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                        rm::sim::VisionStyle::ForgedAlliance);
        UnitStore store;
        (void)place(store, type, 0, 200.0f, 256.0f);
        std::vector<Army> armies = twoArmies(false);
        intel.update(store, catalog, armies, &terrain);
        std::size_t lit = 0;
        for (int x = 0; x < 512; x += 8) {
            for (int z = 0; z < 512; z += 8) {
                lit += intel.sees(0, IntelKind::Vision, Fx::fromInt(x), Fx::fromInt(z)) ? 1u : 0u;
            }
        }
        return lit;
    };

    CHECK(litSquares(lowType) == litSquares(tallType));
    // And the ridge is no obstacle to either, which is the style's whole claim.
    CHECK(litSquares(lowType) > 0);
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

// --- Contacts: what a side knows, and how much of it is true ------------------------

TEST_CASE("an enemy in sight is seen exactly; one on radar alone is a blip") {
    rm::unitdef::UnitDef watcherDef = seer(0.0f, 400.0f);  // radar only, no eyes
    rm::unitdef::UnitDef quietDef = seer(0.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watcherDef);
    const rm::UnitTypeIndex quiet = catalog.add(&quietDef);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId enemy = place(store, quiet, 1, 600.0f, 500.0f);
    std::vector<Army> armies = twoArmies(false);

    intel.update(store, catalog, armies, nullptr);

    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);

    // Its own radar unit, exactly where it is.
    REQUIRE(contacts.size() == 2);
    CHECK(contacts[0].kind == rm::sim::ContactKind::Seen);
    CHECK(contacts[0].x == Fx::fromInt(500));

    // The enemy: known to be somewhere, not known to be anything, and not known to be
    // exactly there.
    const rm::sim::Contact& blip = contacts[1];
    CHECK(blip.unit == enemy);
    CHECK(blip.kind == rm::sim::ContactKind::Radar);
    CHECK(blip.isBlip());
    CHECK(blip.x != Fx::fromInt(600));

    // The error is bounded by what radar is documented to be wrong by, and it is a real
    // displacement rather than a rounding wobble.
    const Fx dx = blip.x - Fx::fromInt(600);
    const Fx dz = blip.z - Fx::fromInt(500);
    const Fx offset = rm::sim::fxSqrt(dx * dx + dz * dz);
    CHECK(offset > Fx::fromInt(1));
    CHECK(offset <= Fx::fromInt(rm::sim::kRadarErrorElmos + 1));
}

TEST_CASE("a radar blip remains after its source dies") {
    rm::unitdef::UnitDef watcherDef = seer(0.0f, 400.0f);  // radar only, no eyes
    rm::unitdef::UnitDef quietDef = seer(0.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watcherDef);
    const rm::UnitTypeIndex quiet = catalog.add(&quietDef);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId enemy = place(store, quiet, 1, 600.0f, 500.0f);
    const std::vector<Army> armies = twoArmies(false);

    intel.update(store, catalog, armies, nullptr);
    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);
    const auto liveBlip = std::find_if(contacts.begin(), contacts.end(),
                                       [&](const rm::sim::Contact& contact) {
                                           return contact.unit == enemy;
                                       });
    REQUIRE(liveBlip != contacts.end());
    CHECK_FALSE(liveBlip->maybeDead);
    const Fx retainedX = liveBlip->x;
    const Fx retainedZ = liveBlip->z;

    store.kill(enemy);
    intel.update(store, catalog, armies, nullptr);

    contacts.clear();
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);
    const auto blip = std::find_if(contacts.begin(), contacts.end(),
                                   [&](const rm::sim::Contact& contact) {
                                       return contact.unit == enemy;
                                   });
    REQUIRE(blip != contacts.end());
    CHECK(blip->kind == rm::sim::ContactKind::Radar);
    CHECK(blip->maybeDead);
    CHECK(blip->x == retainedX);
    CHECK(blip->z == retainedZ);
}

TEST_CASE("a dead blip lingers briefly, then is reaped", "[intel]") {
    rm::unitdef::UnitDef watcherDef = seer(0.0f, 400.0f);
    rm::unitdef::UnitDef quietDef = seer(0.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watcherDef);
    const rm::UnitTypeIndex quiet = catalog.add(&quietDef);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId enemy = place(store, quiet, 1, 600.0f, 500.0f);
    const std::vector<Army> armies = twoArmies(false);

    intel.update(store, catalog, armies, nullptr);
    store.kill(enemy);
    // A battlefield that tidied itself the same tick would lose information a
    // player wants briefly; one that never tidies fills with phantom dots.
    for (int i = 0; i < 50; ++i) {
        intel.update(store, catalog, armies, nullptr);
    }
    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);
    const bool lingering = std::ranges::any_of(contacts, [&](const auto& contact) {
        return contact.unit == enemy;
    });
    CHECK(lingering);
    for (int i = 0; i < 200; ++i) {
        intel.update(store, catalog, armies, nullptr);
    }
    contacts.clear();
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);
    CHECK(std::ranges::none_of(contacts, [&](const auto& contact) {
        return contact.unit == enemy;
    }));
}

TEST_CASE("radar reaping drops a dead contact when its slot is reused") {
    rm::unitdef::UnitDef watcherDef = seer(0.0f, 400.0f);  // radar only, no eyes
    rm::unitdef::UnitDef quietDef = seer(0.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watcherDef);
    const rm::UnitTypeIndex quiet = catalog.add(&quietDef);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId dead = place(store, quiet, 1, 600.0f, 500.0f);
    const std::vector<Army> armies = twoArmies(false);

    intel.update(store, catalog, armies, nullptr);
    store.kill(dead);
    intel.update(store, catalog, armies, nullptr);
    const rm::sim::UnitId replacement = place(store, quiet, 1, 650.0f, 500.0f);
    REQUIRE(replacement.index == dead.index);
    REQUIRE(replacement.generation != dead.generation);

    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);
    CHECK(std::none_of(contacts.begin(), contacts.end(), [dead](const rm::sim::Contact& contact) {
        return contact.unit == dead;
    }));

    intel.update(store, catalog, armies, nullptr);

    const auto retained = intel.retainedRadarContacts(0);
    REQUIRE(retained.size() == 1);
    CHECK(retained.front().unit == replacement);
    CHECK_FALSE(retained.front().maybeDead);
}

TEST_CASE("a radar blip survives a source killed after intel refresh in the same skirmish tick") {
    rm::unitdef::UnitDef watcherDef = seer(0.0f, 400.0f);  // radar only, no eyes
    rm::unitdef::UnitDef quietDef = seer(0.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watcherDef);
    const rm::UnitTypeIndex quiet = catalog.add(&quietDef);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId enemy = place(store, quiet, 1, 600.0f, 500.0f);
    std::vector<Army> armies = twoArmies(false);
    std::vector<rm::sim::Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);
    rm::sim::Match match{.armies = armies,
                          .economies = economies,
                          .commandersEver = commandersEver,
                          .intel = &intel};

    // `tickSkirmish` refreshes Intel before its retirement pass. The already-destroyed target
    // is therefore cached as radar-visible, then killed before the caller projects contacts.
    store.health()[enemy.index].current = rm::sim::magFromFloat(0.0f);
    (void)rm::sim::tickSkirmish(store, catalog, match,
                                 rm::sim::Terrain{flatField(128, 0.0f)});
    REQUIRE_FALSE(store.alive(enemy));

    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);
    const auto blip = std::find_if(contacts.begin(), contacts.end(),
                                   [&](const rm::sim::Contact& contact) {
                                       return contact.unit == enemy;
                                   });
    REQUIRE(blip != contacts.end());
    CHECK(blip->kind == rm::sim::ContactKind::Radar);
}

TEST_CASE("a unit nothing can sense is not a contact at all") {
    // The difference between an intel system and a filter on the draw call: an unseen unit
    // is ABSENT from what a side knows, not present and hidden.
    const rm::unitdef::UnitDef def = seer(80.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, type, 0, 100.0f, 100.0f);
    (void)place(store, type, 1, 900.0f, 900.0f);
    std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);

    REQUIRE(contacts.size() == 1);
    CHECK(contacts[0].kind == rm::sim::ContactKind::Seen);
}

TEST_CASE("your own units are contacts wherever they are") {
    // A lone scout past the edge of every friendly sight radius is still yours. Asking the
    // grid about your own units would lose exactly the unit you sent out to look.
    const rm::unitdef::UnitDef def = seer(0.0f);  // sees nothing, not even itself
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, type, 0, 700.0f, 700.0f);
    std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);

    REQUIRE(contacts.size() == 1);
    CHECK(contacts[0].kind == rm::sim::ContactKind::Seen);
    CHECK(contacts[0].x == Fx::fromInt(700));
}

TEST_CASE("a blip wanders rather than jumping, and is the same wander every run") {
    rm::unitdef::UnitDef watcherDef = seer(0.0f, 400.0f);
    rm::unitdef::UnitDef quietDef = seer(0.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watcherDef);
    const rm::UnitTypeIndex quiet = catalog.add(&quietDef);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    (void)place(store, quiet, 1, 600.0f, 500.0f);
    std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    const auto blipAt = [&](rm::TickIndex tick) {
        std::vector<rm::sim::Contact> contacts;
        rm::sim::contactsFor(0, store, catalog, armies, intel, tick, contacts);
        REQUIRE(contacts.size() == 2);
        return contacts[1];
    };

    // Consecutive ticks move it a little, not a lot: the drift is interpolated across the
    // bucket rather than re-picked. A jump every fifteenth tick would read as a flickering
    // contact rather than an uncertain one.
    const rm::sim::Contact a = blipAt(20);
    const rm::sim::Contact b = blipAt(21);
    const Fx step = rm::sim::fxSqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    CHECK(step < Fx::fromInt(radarErrorStepBound()));

    // And over a whole bucket it really does move — a "drift" that stood still would pass
    // the check above trivially.
    const rm::sim::Contact later = blipAt(20 + static_cast<rm::TickIndex>(blipDriftTicks()));
    const Fx travelled = rm::sim::fxSqrt((later.x - a.x) * (later.x - a.x)
                                         + (later.z - a.z) * (later.z - a.z));
    CHECK(travelled > Fx::fromInt(1));

    // Same tick, same answer. Deterministic without any stored error vector to keep in sync.
    CHECK(blipAt(20).x == a.x);
    CHECK(blipAt(20).z == a.z);
}

// --- Stealth, cloak, omni and free intel --------------------------------------
//
// The four FA intel types that are a FLAG or a fourth grid rather than a second kind of grid.
// The two stealth FIELDS and the jammer act on OTHER units, so they are still out — see
// `IntelKind`'s note — and `CloakFieldRadius` turns out not to exist in retail at all.

namespace {

/// A unit that hides from one sense, or from all of them.
[[nodiscard]] rm::unitdef::UnitDef hider(bool radarStealth, bool sonarStealth, bool cloak) {
    rm::unitdef::UnitDef def;
    def.radarStealth = radarStealth;
    def.sonarStealth = sonarStealth;
    def.cloak = cloak;
    return def;
}

/// A watcher with a sense of each kind.
[[nodiscard]] rm::unitdef::UnitDef watcher(float vision, float radar, float sonar, float omni) {
    rm::unitdef::UnitDef def;
    def.visionRadiusElmos = vision;
    def.radarRadiusElmos = radar;
    def.sonarRadiusElmos = sonar;
    def.omniRadiusElmos = omni;
    return def;
}

/// One alliance's view of a scene: everything it knows about, this tick.
[[nodiscard]] std::vector<rm::sim::Contact> seenBy(int alliance, const UnitStore& store,
                                                   const UnitCatalog& catalog,
                                                   std::span<const Army> armies,
                                                   const Intel& intel) {
    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(alliance, store, catalog, armies, intel, 0, contacts);
    return contacts;
}

} // namespace

TEST_CASE("a radar-stealthed unit is absent from radar, not merely harder to find") {
    UnitCatalog catalog;
    // A STATIC, like every other definition here: the catalog holds POINTERS and a temporary
    // would dangle the moment the call returned. The first draft of this line wrote
    // `&*std::make_unique<...>(...)`, which is that bug spelled at its most confident.
    static const rm::unitdef::UnitDef kEye = watcher(0.0f, 400.0f, 0.0f, 0.0f);
    const rm::UnitTypeIndex eye = catalog.add(&kEye);
    static const rm::unitdef::UnitDef kQuiet = hider(true, false, false);
    static const rm::unitdef::UnitDef kLoud = hider(false, false, false);
    const rm::UnitTypeIndex quiet = catalog.add(&kQuiet);
    const rm::UnitTypeIndex loud = catalog.add(&kLoud);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512), rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, eye, 0, 100.0f, 100.0f);
    (void)place(store, quiet, 1, 140.0f, 100.0f);
    (void)place(store, loud, 1, 160.0f, 100.0f);

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    // Both are well inside a 400-elmo radar. Only the one that does not hide from it returns.
    const auto contacts = seenBy(0, store, catalog, armies, intel);
    std::size_t hostile = 0;
    for (const rm::sim::Contact& contact : contacts) {
        if (contact.isBlip()) {
            ++hostile;
        }
    }
    CHECK(hostile == 1);
}

TEST_CASE("a cloaked unit is absent from sight") {
    UnitCatalog catalog;
    static const rm::unitdef::UnitDef kEye = watcher(400.0f, 0.0f, 0.0f, 0.0f);
    static const rm::unitdef::UnitDef kCloaked = hider(false, false, true);
    const rm::UnitTypeIndex eye = catalog.add(&kEye);
    const rm::UnitTypeIndex cloaked = catalog.add(&kCloaked);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512), rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, eye, 0, 100.0f, 100.0f);
    (void)place(store, cloaked, 1, 140.0f, 100.0f);

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    // Only the watcher's own unit comes back — the cloaked one is not seen and no other sense
    // reaches it, so it is ABSENT rather than a blip.
    const auto contacts = seenBy(0, store, catalog, armies, intel);
    CHECK(contacts.size() == 1);
}

TEST_CASE("a cloaked unit still blips on radar — cloak is anti-vision only", "[intel]") {
    // C-277's retail semantics, and this test previously pinned the WRONG ones: it
    // asserted a cloaked unit was absent from radar too. Retail's flag filter clears
    // `LOSNow` on self-cloak and leaves the radar and sonar bits alone — cloak hides a
    // unit from EYES, not from the dish. What radar returns is a blip, not an
    // identification: detection without identity, like any radar contact.
    UnitCatalog catalog;
    static const rm::unitdef::UnitDef kEye = watcher(0.0f, 400.0f, 0.0f, 0.0f);
    static const rm::unitdef::UnitDef kCloaked = hider(false, false, true);
    const rm::UnitTypeIndex eye = catalog.add(&kEye);
    const rm::UnitTypeIndex cloaked = catalog.add(&kCloaked);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512), rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, eye, 0, 100.0f, 100.0f);
    const rm::sim::UnitId ghost = place(store, cloaked, 1, 140.0f, 100.0f);

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    // Inside a 400-elmo radar: a blip, and never a sighting — the cloak still defeats
    // vision, so nothing here can identify it.
    const auto contacts = seenBy(0, store, catalog, armies, intel);
    const auto blip = std::ranges::find_if(
        contacts, [&](const rm::sim::Contact& c) { return c.unit == ghost; });
    REQUIRE(blip != contacts.end());
    CHECK(blip->kind == rm::sim::ContactKind::Radar);
    CHECK(blip->isBlip());
    CHECK_FALSE(intel.hasSeenEver(0, ghost));
}

TEST_CASE("omni detects a cloaked unit, but does not identify it") {
    // THE WHOLE POINT OF OMNI BEING A SENSE RATHER THAN A BIG VISION RADIUS: it defeats
    // every flag. What it returns is detection WITHOUT identification (`C-280`) — a
    // radar-class blip, not a sighting — and this test previously pinned the wrong half
    // by asserting `Seen` at the exact position.
    UnitCatalog catalog;
    static const rm::unitdef::UnitDef kEye = watcher(0.0f, 0.0f, 0.0f, 400.0f);
    static const rm::unitdef::UnitDef kHidden = hider(true, true, true);
    const rm::UnitTypeIndex eye = catalog.add(&kEye);
    const rm::UnitTypeIndex hidden = catalog.add(&kHidden);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512), rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, eye, 0, 100.0f, 100.0f);
    const rm::sim::UnitId ghost = place(store, hidden, 1, 140.0f, 100.0f);

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    const auto contacts = seenBy(0, store, catalog, armies, intel);
    bool found = false;
    for (const rm::sim::Contact& contact : contacts) {
        if (contact.unit == ghost) {
            found = true;
            CHECK(contact.kind == rm::sim::ContactKind::Radar);
            CHECK(contact.isBlip());
        }
    }
    CHECK(found);
    // And the identification latch stays clear — omni never sets `LOSEver`.
    CHECK_FALSE(intel.hasSeenEver(0, ghost));
}

TEST_CASE("omni does not reach past its own radius") {
    // The counter-test to the one above: it defeats stealth, not distance.
    UnitCatalog catalog;
    static const rm::unitdef::UnitDef kEye = watcher(0.0f, 0.0f, 0.0f, 80.0f);
    static const rm::unitdef::UnitDef kHidden = hider(true, true, true);
    const rm::UnitTypeIndex eye = catalog.add(&kEye);
    const rm::UnitTypeIndex hidden = catalog.add(&kHidden);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512), rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, eye, 0, 100.0f, 100.0f);
    (void)place(store, hidden, 1, 400.0f, 100.0f);

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    CHECK(seenBy(0, store, catalog, armies, intel).size() == 1);
}

TEST_CASE("free intel beats stealth and beats having no sense at all") {
    // A campaign objective does not stop being one because it also carries `RadarStealth`, and
    // 8 blueprints declare `FreeIntel` — some of them beside a stealth flag.
    UnitCatalog catalog;
    static const rm::unitdef::UnitDef kBlind = watcher(0.0f, 0.0f, 0.0f, 0.0f);
    static const rm::unitdef::UnitDef kBeacon = [] {
        rm::unitdef::UnitDef def = hider(true, true, true);
        def.freeIntel = true;
        return def;
    }();
    const rm::UnitTypeIndex blind = catalog.add(&kBlind);
    const rm::UnitTypeIndex beacon = catalog.add(&kBeacon);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512), rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, blind, 0, 100.0f, 100.0f);
    const rm::sim::UnitId marker = place(store, beacon, 1, 400.0f, 400.0f);

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    // Nothing on side 0 can see anything, and the beacon hides from every sense. It is still
    // reported, at its true position.
    const auto contacts = seenBy(0, store, catalog, armies, intel);
    bool found = false;
    for (const rm::sim::Contact& contact : contacts) {
        if (contact.unit == marker) {
            found = true;
            CHECK(contact.kind == rm::sim::ContactKind::Seen);
        }
    }
    CHECK(found);
}

TEST_CASE("a stealth field hides its neighbours from radar, and omni still wins") {
    // The watcher: radar wide enough to cover everything, no sight.
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    // The field generator, and a plain tank standing inside its umbrella.
    rm::unitdef::UnitDef generator;
    generator.radarStealthFieldRadiusElmos = 100.0f;
    rm::unitdef::UnitDef plain;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex field = catalog.add(&generator);
    const rm::UnitTypeIndex tank = catalog.add(&plain);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 200.0f, 200.0f);
    (void)place(store, field, 1, 600.0f, 600.0f);
    const rm::sim::UnitId inside = place(store, tank, 1, 640.0f, 600.0f);
    const rm::sim::UnitId outside = place(store, tank, 1, 900.0f, 600.0f);
    std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);

    // The tank inside the umbrella is ABSENT; the one outside is an ordinary blip. The
    // generator hides under its own umbrella too — FA's do.
    const auto knows = [&](rm::sim::UnitId id) {
        return std::any_of(contacts.begin(), contacts.end(),
                           [&](const rm::sim::Contact& c) { return c.unit == id; });
    };
    CHECK_FALSE(knows(inside));
    CHECK(knows(outside));

    // Omni is the counter: give the watcher omni over the field and the hidden tank is
    // detected — as a BLIP, not a sighting (`C-280`: omni bypasses counter-intel but
    // does not identify; this assertion previously pinned `Seen`, the wrong half).
    rm::unitdef::UnitDef allSeeing = seer(0.0f, 800.0f);
    allSeeing.omniRadiusElmos = 800.0f;
    UnitCatalog counters;
    const rm::UnitTypeIndex omni = counters.add(&allSeeing);
    const rm::UnitTypeIndex field2 = counters.add(&generator);
    const rm::UnitTypeIndex tank2 = counters.add(&plain);

    Intel intel2;
    intel2.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                     rm::sim::VisionStyle::ForgedAlliance);
    UnitStore store2;
    (void)place(store2, omni, 0, 200.0f, 200.0f);
    (void)place(store2, field2, 1, 600.0f, 600.0f);
    const rm::sim::UnitId inside2 = place(store2, tank2, 1, 640.0f, 600.0f);
    intel2.update(store2, counters, armies, nullptr);

    contacts.clear();
    rm::sim::contactsFor(0, store2, counters, armies, intel2, 0, contacts);
    const auto seen = std::find_if(contacts.begin(), contacts.end(),
                                   [&](const rm::sim::Contact& c) { return c.unit == inside2; });
    REQUIRE(seen != contacts.end());
    CHECK(seen->kind == rm::sim::ContactKind::Radar);
    CHECK(seen->isBlip());
}

TEST_CASE("a jammer scatters false blips inside hostile radar, and only there") {
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    rm::unitdef::UnitDef deceiver;
    deceiver.jamRadiusElmos = 208.0f;  // the retail 26 ogrids
    deceiver.jammerBlips = 10;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex jammer = catalog.add(&deceiver);

    Intel intel;
    intel.configure(2, Fx::fromInt(4096), Fx::fromInt(4096),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId carrier = place(store, jammer, 1, 700.0f, 500.0f);
    std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);

    // The watcher's own contact, the carrier's real blip, and ten lies — every lie keyed
    // to the carrier (a blip must never leak an identity a real one would not).
    std::size_t liesAndCarrier = 0;
    for (const rm::sim::Contact& contact : contacts) {
        if (contact.unit == carrier) {
            ++liesAndCarrier;
            CHECK(contact.kind == rm::sim::ContactKind::Radar);
        }
    }
    CHECK(liesAndCarrier == 11);

    // Out of radar's reach, the deception has no sense to deceive: nothing at all.
    UnitStore far;
    (void)place(far, watcher, 0, 100.0f, 100.0f);
    (void)place(far, jammer, 1, 3000.0f, 3000.0f);
    Intel intel2;
    intel2.configure(2, Fx::fromInt(4096), Fx::fromInt(4096),
                     rm::sim::VisionStyle::ForgedAlliance);
    intel2.update(far, catalog, armies, nullptr);
    contacts.clear();
    rm::sim::contactsFor(0, far, catalog, armies, intel2, 0, contacts);
    CHECK(contacts.size() == 1);  // the watcher itself, and no ghosts
}

TEST_CASE("a cheating army's commander sees the whole map, and only the commander",
          "[intel][cheat]") {
    // C-360's `IntelCheat` (`CheatBuffs.lua`): `ApplyCheatBuffs` gives COMMAND units
    // VisionRadius +10000 and OmniRadius +10000 — the AIx's map-wide sight. The buff is
    // per unit, not per army: the cheating army's ordinary units keep their own radii.
    rm::unitdef::UnitDef commander = seer(26.0f);  // the ACU's own sight radius
    commander.name = "UEL0001";
    commander.categories = {"COMMAND"};
    rm::unitdef::UnitDef tank = seer(26.0f);
    tank.name = "test_tank";
    tank.categories = {"LAND"};

    UnitCatalog catalog;
    const rm::UnitTypeIndex commanderType = catalog.add(&commander);
    const rm::UnitTypeIndex tankType = catalog.add(&tank);

    UnitStore store;
    (void)place(store, commanderType, 0, 256.0f, 256.0f);
    (void)place(store, tankType, 0, 256.0f, 300.0f);
    std::vector<Army> armies = twoArmies(false);
    armies[0].cheatEnabled = true;

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);
    intel.update(store, catalog, armies, nullptr);

    // +10000 elmos of vision and omni covers a 512-elmo map corner to corner.
    const Fx far = Fx::fromInt(500);
    CHECK(intel.sees(0, IntelKind::Vision, far, far));
    CHECK(intel.sees(0, IntelKind::Omni, far, far));

    // The same commander on an honest army sees only its own radius: 500 elmos
    // away is far past 26.
    std::vector<Army> honest = twoArmies(false);
    Intel intel2;
    intel2.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                     rm::sim::VisionStyle::ForgedAlliance);
    intel2.update(store, catalog, honest, nullptr);
    CHECK_FALSE(intel2.sees(0, IntelKind::Vision, far, far));
    CHECK_FALSE(intel2.sees(0, IntelKind::Omni, far, far));

    // And the cheating army's TANK is not a commander: at 500 elmos out it sees
    // nothing either — the buff names COMMAND, not the army.
    UnitStore tankOnly;
    (void)place(tankOnly, tankType, 0, 256.0f, 256.0f);
    Intel intel3;
    intel3.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                     rm::sim::VisionStyle::ForgedAlliance);
    intel3.update(tankOnly, catalog, armies, nullptr);
    CHECK_FALSE(intel3.sees(0, IntelKind::Vision, far, far));
    CHECK_FALSE(intel3.sees(0, IntelKind::Omni, far, far));
}

// --- C-283: per-type intel enable/disable -----------------------------------
//
// Retail keeps an (enabled, active) byte pair per intel type on the unit's
// attributes object — Jammer `+0x24`, Spoof `+0x26`, Cloak `+0x28`, RadarStealth
// `+0x2A`, SonarStealth `+0x2C` (`0x00694B60`/`0x00694C56`). `enabled` is the
// toggle `EnableIntel`/`DisableIntel` flips; `active` is whether the intel
// actually contributes this tick — enabled AND powered AND alive. These cases
// pin the observable contract: a disabled type contributes nothing until it is
// re-enabled, and the enabled flags survive a save.

TEST_CASE("C-283: disabling a unit's radar removes its coverage until re-enabled",
          "[intel]") {
    rm::unitdef::UnitDef watching = seer(0.0f, 400.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    const rm::sim::UnitId dish = place(store, watcher, 0, 200.0f, 200.0f);
    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    const Fx far = Fx::fromInt(500);
    const Fx lane = Fx::fromInt(200);
    CHECK(intel.sees(0, IntelKind::Radar, far, lane));
    CHECK(intel.intelActive(store, dish.index, rm::sim::IntelType::Radar));

    // `DisableIntel('Radar')`: the dish stops contributing — the same update
    // withdraws its stamp even though nothing moved, because the enabled mask
    // is part of the placement's change key.
    REQUIRE(store.setIntelEnabled(dish, rm::sim::IntelType::Radar, false));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::Radar));
    intel.update(store, catalog, armies, nullptr);
    CHECK_FALSE(intel.sees(0, IntelKind::Radar, far, lane));
    CHECK_FALSE(intel.intelActive(store, dish.index, rm::sim::IntelType::Radar));

    // `EnableIntel('Radar')` restores it on the next pass.
    REQUIRE(store.setIntelEnabled(dish, rm::sim::IntelType::Radar, true));
    intel.update(store, catalog, armies, nullptr);
    CHECK(intel.sees(0, IntelKind::Radar, far, lane));
    CHECK(intel.intelActive(store, dish.index, rm::sim::IntelType::Radar));
}

TEST_CASE("C-283: a disabled jammer stops lying, and a disabled stealth field "
          "stops hiding", "[intel]") {
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    rm::unitdef::UnitDef deceiver;
    deceiver.jamRadiusElmos = 208.0f;
    deceiver.jammerBlips = 10;
    rm::unitdef::UnitDef generator;
    generator.radarStealthFieldRadiusElmos = 100.0f;
    rm::unitdef::UnitDef plain;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex jammer = catalog.add(&deceiver);
    const rm::UnitTypeIndex field = catalog.add(&generator);
    const rm::UnitTypeIndex tank = catalog.add(&plain);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 200.0f, 200.0f);
    const rm::sim::UnitId carrier = place(store, jammer, 1, 700.0f, 500.0f);
    const rm::sim::UnitId umbrella = place(store, field, 1, 600.0f, 800.0f);
    const rm::sim::UnitId inside = place(store, tank, 1, 640.0f, 800.0f);
    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    const auto contactsOf = [&](rm::sim::UnitId id) {
        std::vector<rm::sim::Contact> contacts;
        rm::sim::contactsFor(0, store, catalog, armies, intel, 0, contacts);
        return std::count_if(contacts.begin(), contacts.end(),
                             [&](const rm::sim::Contact& c) { return c.unit == id; });
    };

    // Ten lies plus the carrier's own blip, and the tank under the umbrella is
    // absent — the baseline the toggles are measured against.
    CHECK(contactsOf(carrier) == 11);
    CHECK(contactsOf(inside) == 0);

    // `DisableIntel('Jammer')`: the lies stop immediately — the contact pass
    // reads the flag live — while the carrier's real blip stays.
    REQUIRE(store.setIntelEnabled(carrier, rm::sim::IntelType::Jammer, false));
    CHECK(contactsOf(carrier) == 1);

    // `DisableIntel('RadarStealthField')`: the umbrella folds on the next
    // intel pass and the tank it hid becomes an ordinary blip.
    REQUIRE(store.setIntelEnabled(umbrella, rm::sim::IntelType::RadarStealthField,
                                  false));
    intel.update(store, catalog, armies, nullptr);
    CHECK(contactsOf(inside) == 1);

    // Re-enabling restores both contributions.
    REQUIRE(store.setIntelEnabled(carrier, rm::sim::IntelType::Jammer, true));
    REQUIRE(store.setIntelEnabled(umbrella, rm::sim::IntelType::RadarStealthField,
                                  true));
    intel.update(store, catalog, armies, nullptr);
    CHECK(contactsOf(carrier) == 11);
    CHECK(contactsOf(inside) == 0);
}

TEST_CASE("C-283: the RULEUTC toggles write the same enabled mask", "[intel]") {
    // `Unit.lua`'s `OnScriptBitSet` maps the toggle bits onto per-type
    // `DisableUnitIntel` calls — bit 2 the jammer, bit 3 every non-vision type,
    // bit 5 the stealth family, bit 8 the cloak. The sim's mask is the union
    // the refcounted `IntelDisables` table converges to.
    rm::unitdef::UnitDef watching = seer(0.0f, 400.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);

    UnitStore store;
    const rm::sim::UnitId dish = place(store, watcher, 0, 200.0f, 200.0f);

    REQUIRE(store.setScriptBitDisabled(dish, 3, true));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::Radar));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::Omni));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::Jammer));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::Cloak));
    // Vision is not in bit 3's group — retail's intel toggle leaves it on.
    CHECK(store.intelEnabled(dish, rm::sim::IntelType::Vision));

    REQUIRE(store.setScriptBitDisabled(dish, 3, false));
    CHECK(store.intelEnabled(dish, rm::sim::IntelType::Radar));

    // Bit 5 is the stealth family only: radar stays enabled while both
    // stealths and both fields go dark.
    REQUIRE(store.setScriptBitDisabled(dish, 5, true));
    CHECK(store.intelEnabled(dish, rm::sim::IntelType::Radar));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::RadarStealth));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::SonarStealth));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::RadarStealthField));
    CHECK_FALSE(store.intelEnabled(dish, rm::sim::IntelType::SonarStealthField));
}

TEST_CASE("C-283: intel enable flags survive a save", "[intel]") {
    UnitStore original;
    UnitStore::Spawn spawn;
    spawn.motion.armyIndex = 0;
    spawn.health.current = rm::sim::magFromFloat(100.0f);
    spawn.health.maximum = spawn.health.current;
    const rm::sim::UnitId unit = original.spawn(spawn);
    REQUIRE(original.setIntelEnabled(unit, rm::sim::IntelType::Radar, false));
    REQUIRE(original.setIntelEnabled(unit, rm::sim::IntelType::Jammer, false));

    rm::sim::RandomStream random{std::uint32_t{1}};
    const rm::sim::SaveState state{.tick = 7, .random = random.snapshot(),
                                   .units = original.snapshot()};
    const auto saved = rm::sim::SaveState::encode(state);
    const auto restored = rm::sim::SaveState::decode(saved);
    REQUIRE(restored.has_value());

    const UnitStore resumed{restored->units};
    CHECK_FALSE(resumed.intelEnabled(unit, rm::sim::IntelType::Radar));
    CHECK_FALSE(resumed.intelEnabled(unit, rm::sim::IntelType::Jammer));
    CHECK(resumed.intelEnabled(unit, rm::sim::IntelType::Sonar));
    CHECK(rm::sim::SaveState::encode(*restored) == saved);
}

TEST_CASE("C-282: intel bit edges emit IntelChanged and the first blip DetectedBy",
          "[intel]") {
    // `C-282` (`0x005D1E30`, `0x005D1F30`): `CIntel::Update` writes four recon
    // bits per blip — Radar, Sonar, Omni, LOSNow — and every bit's edge is one
    // `OnIntelChange(blip, type, val)`; a blip's birth is `OnDetectedBy(army)`
    // once per army in the viewing alliance.
    const rm::unitdef::UnitDef def = seer(80.0f, 200.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, type, 0, 256.0f, 256.0f);          // army 0's seer
    const rm::sim::UnitId enemy = place(store, type, 1, 400.0f, 256.0f);
    std::vector<Army> armies = twoArmies(false);
    rm::sim::EventQueue events;

    const auto count = [&](rm::sim::EventKind kind) { return events.count(kind); };
    const auto intelChanges = [&](rm::sim::IntelType type, bool value) {
        std::size_t n = 0;
        for (const rm::sim::Event& event : events.all()) {
            if (event.kind == rm::sim::EventKind::IntelChanged
                && event.intelType == static_cast<std::uint8_t>(type)
                && event.intelValue == value && event.unit == enemy
                && event.army == 0) {
                ++n;
            }
        }
        return n;
    };
    const auto detectedBy = [&] {
        std::size_t n = 0;
        for (const rm::sim::Event& event : events.all()) {
            if (event.kind == rm::sim::EventKind::DetectedBy && event.unit == enemy
                && event.army == 0) {
                ++n;
            }
        }
        return n;
    };

    // First pass: the enemy is a radar blip only for army 0 — outside the
    // 80-elmo vision radius, inside the 200-elmo dish. Radar true, DetectedBy
    // once, no LOS. (Every unit also blips for its OWN alliance — own units are
    // always visually known — so the counts filter to the enemy viewed by 0.)
    intel.update(store, catalog, armies, nullptr, rm::sim::TickRate{}, {}, &events);
    CHECK(detectedBy() == 1);
    CHECK(intelChanges(rm::sim::IntelType::Radar, true) == 1);
    CHECK(intelChanges(rm::sim::IntelType::Vision, true) == 0);
    CHECK(intelChanges(rm::sim::IntelType::Sonar, true) == 0);
    CHECK(intelChanges(rm::sim::IntelType::Omni, true) == 0);
    for (const rm::sim::Event& event : events.all()) {
        if ((event.kind == rm::sim::EventKind::DetectedBy
             || event.kind == rm::sim::EventKind::IntelChanged)
            && event.unit == enemy && event.army == 0) {
            CHECK(event.army == 0);  // the VIEWING army, not the owner
        }
    }

    // Steady state: a second pass with nothing changed emits nothing.
    events.beginFrame(1);
    intel.update(store, catalog, armies, nullptr, rm::sim::TickRate{}, {}, &events);
    CHECK(events.empty());

    // The unit steps into vision: LOSNow rises — one more IntelChanged, and no
    // second DetectedBy (the blip already exists).
    events.beginFrame(2);
    store.transforms()[enemy.index].x = rm::sim::fxFromFloat(290.0f);
    intel.update(store, catalog, armies, nullptr, rm::sim::TickRate{}, {}, &events);
    CHECK(count(rm::sim::EventKind::DetectedBy) == 0);
    CHECK(intelChanges(rm::sim::IntelType::Vision, true) == 1);
    CHECK(intelChanges(rm::sim::IntelType::Radar, true) == 0);

    // It walks out of both radii: both bits fall, two IntelChanged(false).
    events.beginFrame(3);
    store.transforms()[enemy.index].x = rm::sim::fxFromFloat(500.0f);
    intel.update(store, catalog, armies, nullptr, rm::sim::TickRate{}, {}, &events);
    CHECK(intelChanges(rm::sim::IntelType::Vision, false) == 1);
    CHECK(intelChanges(rm::sim::IntelType::Radar, false) == 1);
}

TEST_CASE("C-282: a dying unit's blip falls bit by bit", "[intel]") {
    const rm::unitdef::UnitDef def = seer(80.0f, 200.0f);
    UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&def);

    Intel intel;
    intel.configure(2, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, type, 0, 256.0f, 256.0f);
    const rm::sim::UnitId enemy = place(store, type, 1, 400.0f, 256.0f);
    std::vector<Army> armies = twoArmies(false);
    rm::sim::EventQueue events;

    intel.update(store, catalog, armies, nullptr, rm::sim::TickRate{}, {}, &events);
    // Four blip births in all — each unit for each alliance — but the enemy's
    // for army 0 exactly once.
    std::size_t enemyDetected = 0;
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == rm::sim::EventKind::DetectedBy && event.unit == enemy
            && event.army == 0) {
            ++enemyDetected;
        }
    }
    REQUIRE(enemyDetected == 1);

    // The blip dies: its stored bits fall to zero — the same per-bit diff as
    // walking out of range, which is what retail's blip-destroyed path emits.
    events.beginFrame(1);
    store.kill(enemy);
    intel.update(store, catalog, armies, nullptr, rm::sim::TickRate{}, {}, &events);
    std::size_t falses = 0;
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == rm::sim::EventKind::IntelChanged && event.unit == enemy
            && event.army == 0 && !event.intelValue) {
            ++falses;
        }
    }
    CHECK(falses == 1);  // only Radar was set for army 0
}
