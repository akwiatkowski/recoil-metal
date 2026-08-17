#pragma once

// A minimal S3O writer for loader fixtures.
//
// Same rationale as SmfWriter: assets are never committed, so fixtures have to
// be synthesised, and known-answer tests need the test to choose the bytes.
//
// The standing caveat applies here too — a writer and a parser built from one
// reading of the spec can share a misunderstanding and still agree. That is why
// tests/test_real_model.cpp exercises the 127 real BAR models; these tests prove
// self-consistency, those prove correctness.
//
// Unlike SmfWriter this is hierarchy-shaped: pieces are declared as a flat list
// with parent indices, and the writer lays out the tree, string table, vertex
// arrays and index tables and resolves every offset.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rmtest {

struct S3oVertex {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
};

struct S3oPiece {
    std::string name = "piece";
    int parent = -1;  ///< index into S3oSpec::pieces, -1 for the root

    float xoffset = 0.0f, yoffset = 0.0f, zoffset = 0.0f;

    std::int32_t primitiveType = 0;  ///< 0 triangles, 1 strip, 2 quads
    std::vector<S3oVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct S3oSpec {
    float radius = 0.0f;
    float height = 0.0f;
    float midx = 0.0f, midy = 0.0f, midz = 0.0f;

    std::string texture1 = "unit_diffuse.dds";
    std::string texture2 = "unit_other.dds";

    /// Piece 0 is the root. Parents must precede their children.
    std::vector<S3oPiece> pieces;

    // --- Knobs for negative tests ------------------------------------------
    std::int32_t version = 0;
    bool validMagic = true;
    /// Point the root piece's first child back at the root, making a cycle.
    bool cyclicChild = false;
    /// Point the root piece's vertex array past the end of the file.
    bool vertexOffsetPastEnd = false;
};

/// Serialises spec into a complete .s3o byte image.
[[nodiscard]] std::vector<std::byte> writeS3o(const S3oSpec& spec);

/// A two-piece model: a root with one triangle and a child with one triangle,
/// offset so the hierarchy accumulation is observable.
[[nodiscard]] S3oSpec twoPieceSpec();

} // namespace rmtest
