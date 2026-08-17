// SMT tile-file loader tests. Fixtures are built inline: the format is a
// 32-byte header followed by fixed-size tiles, small enough that a helper here
// beats a separate writer module.
#include <catch2/catch_test_macros.hpp>

#include "core/map/Smt.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

void appendI32(std::vector<std::byte>& out, std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    out.push_back(static_cast<std::byte>(u & 0xFFu));
    out.push_back(static_cast<std::byte>((u >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>((u >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((u >> 24) & 0xFFu));
}

struct SmtSpec {
    std::int32_t numTiles = 2;
    std::int32_t version = 1;
    std::int32_t tileSize = 32;
    std::int32_t compression = 1;
    bool validMagic = true;
    /// Tiles actually written; defaults to numTiles worth.
    std::int32_t tilesWritten = -1;
};

[[nodiscard]] std::vector<std::byte> writeSmt(const SmtSpec& spec) {
    std::vector<std::byte> out;

    const char* magic = "spring tilefile";
    for (std::size_t i = 0; i < 16; ++i) {
        out.push_back(spec.validMagic ? static_cast<std::byte>(magic[i]) : std::byte{0});
    }
    appendI32(out, spec.version);
    appendI32(out, spec.numTiles);
    appendI32(out, spec.tileSize);
    appendI32(out, spec.compression);

    const std::int32_t written = spec.tilesWritten < 0 ? spec.numTiles : spec.tilesWritten;
    for (std::int32_t tile = 0; tile < written; ++tile) {
        // Fill each tile with its own index so mip slicing is checkable.
        for (std::size_t b = 0; b < rm::smt::kTileBytes; ++b) {
            out.push_back(static_cast<std::byte>((static_cast<std::size_t>(tile) + b) & 0xFF));
        }
    }
    return out;
}

} // namespace

TEST_CASE("a valid SMT round-trips its tile count and payload") {
    const auto bytes = writeSmt(SmtSpec{});
    const auto set = rm::smt::load(bytes);

    REQUIRE(set.has_value());
    REQUIRE(set->tileCount == 2);
    REQUIRE(set->tiles.size() == 2 * rm::smt::kTileBytes);
}

TEST_CASE("the four mip levels slice at the engine's offsets") {
    // TILE_MIP_OFFSET = {0, 512, 640, 672}, sizes {512, 128, 32, 8}, summing to
    // SMALL_TILE_SIZE = 680 (SMFFormat.h:28, SMFGroundTextures.cpp:515).
    const auto bytes = writeSmt(SmtSpec{});
    const auto set = rm::smt::load(bytes);
    REQUIRE(set.has_value());

    std::size_t total = 0;
    for (int level = 0; level < rm::smt::kMipLevels; ++level) {
        const auto mip = set->mip(0, level);
        REQUIRE(mip.size() == rm::smt::kMipBytes[level]);
        total += mip.size();
    }
    REQUIRE(total == rm::smt::kTileBytes);

    // Level 0 starts at the tile's first byte; level 1 at byte 512.
    REQUIRE(set->mip(0, 0).data() == set->tiles.data());
    REQUIRE(set->mip(0, 1).data() == set->tiles.data() + 512);

    // Tile 1 begins one whole tile later.
    REQUIRE(set->mip(1, 0).data() == set->tiles.data() + rm::smt::kTileBytes);
}

TEST_CASE("out-of-range tile or mip requests yield an empty span, not UB") {
    const auto bytes = writeSmt(SmtSpec{});
    const auto set = rm::smt::load(bytes);
    REQUIRE(set.has_value());

    REQUIRE(set->mip(-1, 0).empty());
    REQUIRE(set->mip(2, 0).empty());
    REQUIRE(set->mip(0, -1).empty());
    REQUIRE(set->mip(0, rm::smt::kMipLevels).empty());
}

TEST_CASE("a file without the tilefile magic is rejected") {
    SmtSpec spec;
    spec.validMagic = false;

    const auto set = rm::smt::load(writeSmt(spec));

    REQUIRE_FALSE(set.has_value());
    REQUIRE(set.error().code == rm::MapError::Code::NotSmf);
}

TEST_CASE("SMT header fields the engine pins are enforced") {
    // Exactly the checks at SMFGroundTextures.cpp:161-168.
    SmtSpec spec;

    SECTION("version must be 1") { spec.version = 2; }
    SECTION("tile size must be 32") { spec.tileSize = 16; }
    SECTION("compression must be DXT1") { spec.compression = 2; }

    const auto set = rm::smt::load(writeSmt(spec));

    REQUIRE_FALSE(set.has_value());
    REQUIRE(set.error().code == rm::MapError::Code::BadHeader);
}

TEST_CASE("a truncated payload is rejected rather than partially accepted") {
    SmtSpec spec;
    spec.numTiles = 4;
    spec.tilesWritten = 2;  // header lies about how much data follows

    const auto set = rm::smt::load(writeSmt(spec));

    REQUIRE_FALSE(set.has_value());
    REQUIRE(set.error().code == rm::MapError::Code::Truncated);
}

TEST_CASE("a header-only file with zero tiles is valid") {
    SmtSpec spec;
    spec.numTiles = 0;

    const auto set = rm::smt::load(writeSmt(spec));

    REQUIRE(set.has_value());
    REQUIRE(set->tileCount == 0);
    REQUIRE(set->tiles.empty());
}

TEST_CASE("an SMT shorter than its header is rejected") {
    auto bytes = writeSmt(SmtSpec{});
    bytes.resize(rm::smt::kHeaderSize - 1);

    const auto set = rm::smt::load(bytes);

    REQUIRE_FALSE(set.has_value());
    REQUIRE(set.error().code == rm::MapError::Code::Truncated);
}
