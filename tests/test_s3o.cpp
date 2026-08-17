// S3O loader tests. Fixtures come from tests/support/S3oWriter; the 127 real BAR
// models are exercised in tests/test_real_model.cpp, which is what actually
// proves the spec was read correctly.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/model/S3o.hpp"
#include "support/S3oWriter.hpp"

#include <cstdint>
#include <string>
#include <vector>

using Catch::Approx;
using rmtest::S3oPiece;
using rmtest::S3oSpec;
using rmtest::S3oVertex;

TEST_CASE("a two-piece model round-trips its hierarchy") {
    const auto model = rm::s3o::load(rmtest::writeS3o(rmtest::twoPieceSpec()));

    REQUIRE(model.has_value());
    REQUIRE(model->bones.size() == 2);
    REQUIRE(model->bones[0].name == "base");
    REQUIRE(model->bones[1].name == "turret");

    // The root has no parent; the child points back at index 0.
    REQUIRE(model->bones[0].parent == -1);
    REQUIRE(model->bones[1].parent == 0);
}

TEST_CASE("bone offsets accumulate down the hierarchy") {
    // Root at (1,2,3), child at (10,20,30) relative to it. The engine sums these
    // into a global offset (S3OParser.cpp:163) — there is no rotation in the
    // format, so addition is the whole transform.
    const auto model = rm::s3o::load(rmtest::writeS3o(rmtest::twoPieceSpec()));
    REQUIRE(model.has_value());

    REQUIRE(model->bones[0].offset[0] == Approx(1.0f));
    REQUIRE(model->bones[0].globalOffset[1] == Approx(2.0f));

    REQUIRE(model->bones[1].offset[0] == Approx(10.0f));
    REQUIRE(model->bones[1].globalOffset[0] == Approx(11.0f));
    REQUIRE(model->bones[1].globalOffset[1] == Approx(22.0f));
    REQUIRE(model->bones[1].globalOffset[2] == Approx(33.0f));
}

TEST_CASE("every vertex names the bone whose piece it came from") {
    // This is the property that lets .scm drop into the same struct later: S3O
    // partitions geometry per piece, so the bone index is per-piece constant.
    const auto model = rm::s3o::load(rmtest::writeS3o(rmtest::twoPieceSpec()));
    REQUIRE(model.has_value());
    REQUIRE(model->vertices.size() == 6);

    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(model->vertices[i].boneIndex == 0);
    }
    for (std::size_t i = 3; i < 6; ++i) {
        REQUIRE(model->vertices[i].boneIndex == 1);
    }
}

TEST_CASE("per-piece indices are rebased onto the merged vertex array") {
    // Both pieces number their own vertices 0,1,2. Merged, the second piece's
    // triangle must address 3,4,5 — getting this wrong draws the first piece
    // twice, which looks plausible and is completely wrong.
    const auto model = rm::s3o::load(rmtest::writeS3o(rmtest::twoPieceSpec()));
    REQUIRE(model.has_value());
    REQUIRE(model->indices.size() == 6);

    REQUIRE(model->indices[0] == 0);
    REQUIRE(model->indices[1] == 1);
    REQUIRE(model->indices[2] == 2);
    REQUIRE(model->indices[3] == 3);
    REQUIRE(model->indices[4] == 4);
    REQUIRE(model->indices[5] == 5);
}

TEST_CASE("vertex attributes survive the trip") {
    const auto model = rm::s3o::load(rmtest::writeS3o(rmtest::twoPieceSpec()));
    REQUIRE(model.has_value());

    REQUIRE(model->vertices[1].position[0] == Approx(10.0f));
    REQUIRE(model->vertices[1].uv[0] == Approx(1.0f));
    REQUIRE(model->vertices[1].normal[1] == Approx(1.0f));

    // The child's normals point along +X.
    REQUIRE(model->vertices[3].normal[0] == Approx(1.0f));
}

TEST_CASE("normals are normalised, and non-finite ones are zeroed not propagated") {
    S3oSpec spec;
    S3oPiece piece;
    piece.name = "root";
    piece.vertices = {
        // Length 5 — must come back unit length.
        S3oVertex{0.0f, 0.0f, 0.0f, 3.0f, 4.0f, 0.0f, 0.0f, 0.0f},
        // Infinity: the engine zeroes these rather than poisoning lighting
        // (S3OParser.cpp:143-147). A zero normal shades flat; a NaN spreads.
        S3oVertex{1.0f, 0.0f, 0.0f, std::numeric_limits<float>::infinity(), 0.0f, 0.0f, 0.0f, 0.0f},
        S3oVertex{0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    };
    piece.indices = {0, 1, 2};
    spec.pieces = {piece};

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());

    REQUIRE(model->vertices[0].normal[0] == Approx(0.6f));
    REQUIRE(model->vertices[0].normal[1] == Approx(0.8f));

    REQUIRE(model->vertices[1].normal[0] == Approx(0.0f));
    REQUIRE(model->vertices[1].normal[1] == Approx(0.0f));

    // An all-zero normal is left alone rather than divided by zero.
    REQUIRE(model->vertices[2].normal[1] == Approx(0.0f));
}

TEST_CASE("quads are split into two triangles") {
    // 4 indices become 6, as 0-1-2 and 0-2-3 (S3OParser.cpp:237-245).
    S3oSpec spec;
    S3oPiece piece;
    piece.primitiveType = 2;
    piece.vertices = {S3oVertex{}, S3oVertex{}, S3oVertex{}, S3oVertex{}};
    piece.indices = {0, 1, 2, 3};
    spec.pieces = {piece};

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());
    REQUIRE(model->indices.size() == 6);
    REQUIRE(model->indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
}

TEST_CASE("a quad table with a stray index is discarded whole") {
    // The engine drops the piece's geometry rather than guessing
    // (S3OParser.cpp:227-231).
    S3oSpec spec;
    S3oPiece piece;
    piece.primitiveType = 2;
    piece.vertices = {S3oVertex{}, S3oVertex{}, S3oVertex{}, S3oVertex{}, S3oVertex{}};
    piece.indices = {0, 1, 2, 3, 4};  // not a multiple of 4
    spec.pieces = {piece};

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());
    REQUIRE(model->indices.empty());
    // The vertices are still loaded; only the index table is rejected.
    REQUIRE(model->vertices.size() == 5);
}

TEST_CASE("triangle strips emit every consecutive triple") {
    S3oSpec spec;
    S3oPiece piece;
    piece.primitiveType = 1;
    piece.vertices = {S3oVertex{}, S3oVertex{}, S3oVertex{}, S3oVertex{}};
    piece.indices = {0, 1, 2, 3};
    spec.pieces = {piece};

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());

    // Two triangles: (0,1,2) and (1,2,3). Note the winding is NOT alternated —
    // that matches the engine, and the content is authored against the engine.
    REQUIRE(model->indices == std::vector<std::uint32_t>{0, 1, 2, 1, 2, 3});
}

TEST_CASE("strip end markers break the strip instead of emitting garbage") {
    S3oSpec spec;
    S3oPiece piece;
    piece.primitiveType = 1;
    piece.vertices = {S3oVertex{}, S3oVertex{}, S3oVertex{}, S3oVertex{}, S3oVertex{}};
    piece.indices = {0, 1, 2, rm::s3o::kStripEnd, 2, 3, 4};
    spec.pieces = {piece};

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());

    // Only (0,1,2) and (2,3,4) are whole; every triple spanning the marker is
    // skipped. Emitting 0xFFFFFFFF as an index would read far out of bounds.
    REQUIRE(model->indices == std::vector<std::uint32_t>{0, 1, 2, 2, 3, 4});
}

TEST_CASE("out-of-range indices are dropped, not emitted") {
    S3oSpec spec;
    S3oPiece piece;
    piece.vertices = {S3oVertex{}, S3oVertex{}, S3oVertex{}};
    piece.indices = {0, 1, 2, 0, 1, 99};  // second triangle addresses nothing
    spec.pieces = {piece};

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());
    REQUIRE(model->indices.size() == 3);
}

TEST_CASE("texture names are read from the header's offsets") {
    const auto model = rm::s3o::load(rmtest::writeS3o(rmtest::twoPieceSpec()));
    REQUIRE(model.has_value());
    REQUIRE(model->textures[0] == "unit_diffuse.dds");
    REQUIRE(model->textures[1] == "unit_other.dds");
}

TEST_CASE("radius and height fall back to the geometry when the header omits them") {
    // The engine treats <= 0.01 as unsupplied (S3OParser.cpp:79-80).
    auto spec = rmtest::twoPieceSpec();
    spec.radius = 0.0f;
    spec.height = 0.0f;

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());
    REQUIRE(model->radius > 0.0f);
    REQUIRE(model->height > 0.0f);
}

TEST_CASE("a supplied radius and height are kept as written") {
    auto spec = rmtest::twoPieceSpec();
    spec.radius = 42.0f;
    spec.height = 17.0f;

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());
    REQUIRE(model->radius == Approx(42.0f));
    REQUIRE(model->height == Approx(17.0f));
}

TEST_CASE("model bounds account for each bone's accumulated offset") {
    const auto model = rm::s3o::load(rmtest::writeS3o(rmtest::twoPieceSpec()));
    REQUIRE(model.has_value());

    // Root vertices sit at global (1,2,3)..(11,2,13); the child's at
    // (11,22,33)..(16,27,33). The minimum X is therefore the root's origin.
    REQUIRE(model->boundsMin[0] == Approx(1.0f));
    REQUIRE(model->boundsMax[1] == Approx(27.0f));
    REQUIRE(model->boundsMax[2] == Approx(33.0f));
}

TEST_CASE("a file without the S3O magic is rejected") {
    auto spec = rmtest::twoPieceSpec();
    spec.validMagic = false;

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().code == rm::MapError::Code::NotSmf);
}

TEST_CASE("only version 0 is accepted") {
    auto spec = rmtest::twoPieceSpec();
    spec.version = 1;

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().code == rm::MapError::Code::BadHeader);
}

TEST_CASE("a cyclic piece hierarchy is rejected instead of recursing forever") {
    // A corrupt or hostile file can point a child back at an ancestor. Without
    // the visited set this is an infinite recursion, i.e. a stack overflow.
    auto spec = rmtest::twoPieceSpec();
    spec.cyclicChild = true;

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().code == rm::MapError::Code::BadGeometry);
    REQUIRE(model.error().message.find("cyclic") != std::string::npos);
}

TEST_CASE("a vertex array pointing past the end is rejected") {
    // Real .s3o files in the wild do this; the engine works around it
    // (S3OParser.cpp:115-117), so the check is not hypothetical.
    auto spec = rmtest::twoPieceSpec();
    spec.vertexOffsetPastEnd = true;

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().code == rm::MapError::Code::Truncated);
}

TEST_CASE("a file shorter than the header is rejected") {
    auto bytes = rmtest::writeS3o(rmtest::twoPieceSpec());
    bytes.resize(rm::s3o::kHeaderSize - 1);

    const auto model = rm::s3o::load(bytes);
    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().code == rm::MapError::Code::Truncated);
}

TEST_CASE("a single empty piece loads as a valid but geometry-free model") {
    S3oSpec spec;
    spec.pieces = {S3oPiece{}};

    const auto model = rm::s3o::load(rmtest::writeS3o(spec));
    REQUIRE(model.has_value());
    REQUIRE(model->bones.size() == 1);
    REQUIRE(model->empty());
    REQUIRE(model->triangleCount() == 0);
}
