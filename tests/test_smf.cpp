// Known-answer tests for the SMF loader.
//
// Fixtures come from tests/support/SmfWriter, which mirrors Recoil's own map
// writer. That makes these tests self-consistency proofs: they pin the layout
// and the decode formula, but a shared misreading of the spec would satisfy
// both sides. tests/test_real_map.cpp closes that gap against a real map.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/ProceduralField.hpp"
#include "core/map/Smf.hpp"
#include "support/SmfWriter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

using Catch::Approx;
using rmtest::SmfSpec;

namespace {

// The smallest legal map: mapx/mapy must be multiples of the 128-square big
// square, so 128 is the floor. 129 x 129 samples keeps the tests fast.
constexpr std::int32_t kMinMap = 128;

// A deliberately exact vertical range. The span is 4096 elmos over 65536 steps,
// so one raw unit is exactly 1/16 elmo — a power of two, hence no float drift
// and genuinely known answers. These are also FAR's real numbers for a
// converted 20x20 km SupCom map (report 06 section 8.2).
constexpr float kMinHeight = -140.0f;
constexpr float kMaxHeight = 3956.0f;  // kMinHeight + 4096
constexpr float kElmosPerRawUnit = 4096.0f / 65536.0f;  // = 0.0625

[[nodiscard]] SmfSpec baseSpec() {
    SmfSpec spec;
    spec.mapx = kMinMap;
    spec.mapy = kMinMap;
    spec.minHeight = kMinHeight;
    spec.maxHeight = kMaxHeight;
    return spec;
}

// Corner samples for a spec's geometry. Wrapped because the grid is (n+1)^2 and
// the casts are noisy under -Wsign-conversion.
[[nodiscard]] std::size_t cornerCount(const SmfSpec& spec) {
    return static_cast<std::size_t>(spec.mapx + 1) * static_cast<std::size_t>(spec.mapy + 1);
}

/// A zero-filled height grid sized for spec, ready to have samples pinned.
[[nodiscard]] std::vector<std::uint16_t> flatGrid(const SmfSpec& spec) {
    return std::vector<std::uint16_t>(cornerCount(spec), std::uint16_t{0});
}

} // namespace

TEST_CASE("a valid SMF round-trips its geometry") {
    const auto bytes = rmtest::writeSmf(baseSpec());
    const auto field = rm::smf::load(bytes);

    REQUIRE(field.has_value());
    REQUIRE(field->squaresX == kMinMap);
    REQUIRE(field->squaresZ == kMinMap);
    REQUIRE(field->verticesX() == kMinMap + 1);
    REQUIRE(field->verticesZ() == kMinMap + 1);
    REQUIRE(field->raw.size() == field->sampleCount());
}

TEST_CASE("the heightmap is located by its header pointer, not by adjacency") {
    // The writer places a vegetation extra header and a grass map between the
    // header and the heightmap, exactly as Recoil's generator does. A parser
    // that assumed the heightmap follows the 80-byte header would read grass.
    auto spec = baseSpec();
    spec.heights = flatGrid(spec);
    spec.heights[0] = 16;  // 16 raw units = exactly 1 elmo above minHeight

    const auto bytes = rmtest::writeSmf(spec);
    const auto field = rm::smf::load(bytes);

    REQUIRE(field.has_value());
    REQUIRE(field->heightAt(0, 0) == Approx(kMinHeight + 1.0f));
}

TEST_CASE("height decode follows h = min + raw * (max - min) / 65536") {
    auto spec = baseSpec();
    spec.heights = flatGrid(spec);

    // Three pinned points across the domain.
    spec.heights[0] = 0;
    spec.heights[1] = 32768;  // exactly half the domain
    spec.heights[2] = 65535;  // the largest representable word

    const auto bytes = rmtest::writeSmf(spec);
    const auto field = rm::smf::load(bytes);
    REQUIRE(field.has_value());

    // raw 0 lands exactly on minHeight.
    REQUIRE(field->heightAt(0, 0) == Approx(kMinHeight));

    // Half the domain is half the span above minHeight.
    REQUIRE(field->heightAt(1, 0) == Approx(kMinHeight + 2048.0f));

    // The divisor is 65536, NOT 65535 — so the top word falls one quantisation
    // step short of maxHeight and never quite reaches it. This is the detail
    // most reimplementations get wrong.
    REQUIRE(field->heightAt(2, 0) == Approx(kMaxHeight - kElmosPerRawUnit));
    REQUIRE(field->heightAt(2, 0) < kMaxHeight);
}

TEST_CASE("heightAt clamps out-of-range indices to the edge sample") {
    // The mesh builder samples x-1 / z-1 at the borders for its central
    // difference; clamping is what saves it from an edge special case.
    auto spec = baseSpec();
    spec.heights = flatGrid(spec);
    spec.heights[0] = 160;  // 10 elmos above minHeight

    const auto bytes = rmtest::writeSmf(spec);
    const auto field = rm::smf::load(bytes);
    REQUIRE(field.has_value());

    REQUIRE(field->heightAt(-1, 0) == Approx(field->heightAt(0, 0)));
    REQUIRE(field->heightAt(0, -1) == Approx(field->heightAt(0, 0)));
    REQUIRE(field->heightAt(-5, -5) == Approx(field->heightAt(0, 0)));

    const int last = kMinMap;
    REQUIRE(field->heightAt(last + 7, last) == Approx(field->heightAt(last, last)));
}

TEST_CASE("a file without the magic string is rejected as not-SMF") {
    auto spec = baseSpec();
    spec.validMagic = false;

    const auto bytes = rmtest::writeSmf(spec);
    const auto field = rm::smf::load(bytes);

    REQUIRE_FALSE(field.has_value());
    REQUIRE(field.error().code == rm::MapError::Code::NotSmf);
}

TEST_CASE("header fields the engine pins are enforced") {
    // Exactly the four checks in CheckHeader (SMFMapFile.cpp:16-28).
    auto spec = baseSpec();

    SECTION("version must be 1") { spec.version = 2; }
    SECTION("tilesize must be 32") { spec.tilesize = 16; }
    SECTION("texelPerSquare must be 8") { spec.texelPerSquare = 4; }
    SECTION("squareSize must be 8") { spec.squareSize = 16; }

    const auto bytes = rmtest::writeSmf(spec);
    const auto field = rm::smf::load(bytes);

    REQUIRE_FALSE(field.has_value());
    REQUIRE(field.error().code == rm::MapError::Code::BadHeader);
}

TEST_CASE("map dimensions must be positive multiples of the big-square size") {
    // Recoil itself never validates this and degrades silently — the last strip
    // of terrain simply never gets drawn (RoamMeshDrawer.cpp:58-60). We reject
    // loudly instead.
    auto spec = baseSpec();

    SECTION("not a multiple of 128") { spec.mapx = 100; }
    SECTION("zero") { spec.mapy = 0; }
    SECTION("negative") { spec.mapx = -128; }

    const auto bytes = rmtest::writeSmf(spec);
    const auto field = rm::smf::load(bytes);

    REQUIRE_FALSE(field.has_value());
    REQUIRE(field.error().code == rm::MapError::Code::BadGeometry);
}

TEST_CASE("a file too short for its own header is rejected") {
    auto bytes = rmtest::writeSmf(baseSpec());
    bytes.resize(rm::smf::kHeaderSize - 1);

    const auto field = rm::smf::load(bytes);

    REQUIRE_FALSE(field.has_value());
    REQUIRE(field.error().code == rm::MapError::Code::Truncated);
}

TEST_CASE("a file whose heightmap runs past the end is rejected") {
    auto bytes = rmtest::writeSmf(baseSpec());
    // Keep the header intact but lop off most of the payload.
    bytes.resize(rm::smf::kHeaderSize + 64);

    const auto field = rm::smf::load(bytes);

    REQUIRE_FALSE(field.has_value());
    REQUIRE(field.error().code == rm::MapError::Code::Truncated);
}

TEST_CASE("the procedural field fills every grid corner with real relief") {
    const auto field = rm::makeSineHills(kMinMap, kMinMap, kMinHeight, kMaxHeight);

    REQUIRE(field.squaresX == kMinMap);
    REQUIRE(field.raw.size() == field.sampleCount());

    // It must actually have relief, otherwise it is a useless render fixture.
    const auto [lo, hi] = std::minmax_element(field.raw.begin(), field.raw.end());
    REQUIRE(*hi > *lo);
    REQUIRE(*hi - *lo > 30000);

    // And it must decode into the map's declared vertical range.
    REQUIRE(field.heightAt(0, 0) >= kMinHeight);
    REQUIRE(field.heightAt(0, 0) <= kMaxHeight);
}

TEST_CASE("a procedural field survives a write/read round trip through SMF") {
    // The closest thing to an end-to-end check available without a real map:
    // generate relief, serialise it, parse it back, and require every sample to
    // match. Still self-consistency, not correctness — see the file header.
    auto spec = baseSpec();
    const auto source = rm::makeSineHills(spec.mapx, spec.mapy, kMinHeight, kMaxHeight);
    spec.heights = source.raw;

    const auto bytes = rmtest::writeSmf(spec);
    const auto parsed = rm::smf::load(bytes);

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->raw == source.raw);
    REQUIRE(parsed->heightScale == Approx(source.heightScale));
    REQUIRE(parsed->baseHeight == Approx(source.baseHeight));
}
