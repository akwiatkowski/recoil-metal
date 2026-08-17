#include "support/S3oWriter.hpp"

#include <bit>

namespace {

void appendU32(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
}

void appendI32(std::vector<std::byte>& out, std::int32_t v) {
    appendU32(out, std::bit_cast<std::uint32_t>(v));
}

void appendF32(std::vector<std::byte>& out, float v) {
    appendU32(out, std::bit_cast<std::uint32_t>(v));
}

void patchI32(std::vector<std::byte>& out, std::size_t at, std::int32_t v) {
    const auto u = std::bit_cast<std::uint32_t>(v);
    out[at + 0] = static_cast<std::byte>(u & 0xFFu);
    out[at + 1] = static_cast<std::byte>((u >> 8) & 0xFFu);
    out[at + 2] = static_cast<std::byte>((u >> 16) & 0xFFu);
    out[at + 3] = static_cast<std::byte>((u >> 24) & 0xFFu);
}

void appendString(std::vector<std::byte>& out, const std::string& text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    out.push_back(std::byte{0});
}

constexpr std::size_t kHeaderSize = 52;
constexpr std::size_t kPieceSize = 52;

// Byte offsets of the fields the writer patches after laying everything out.
constexpr std::size_t kPieceOffName        = 0;
constexpr std::size_t kPieceOffNumChildren = 4;
constexpr std::size_t kPieceOffChildren    = 8;
constexpr std::size_t kPieceOffVertices    = 16;
constexpr std::size_t kPieceOffTable       = 32;

} // namespace

namespace rmtest {

std::vector<std::byte> writeS3o(const S3oSpec& spec) {
    std::vector<std::byte> out;

    // --- Header ------------------------------------------------------------
    if (spec.validMagic) {
        appendString(out, "Spring unit");  // 11 chars + NUL = the 12-byte field
    } else {
        for (int i = 0; i < 12; ++i) {
            out.push_back(std::byte{0});
        }
    }
    appendI32(out, spec.version);
    appendF32(out, spec.radius);
    appendF32(out, spec.height);
    appendF32(out, spec.midx);
    appendF32(out, spec.midy);
    appendF32(out, spec.midz);

    const std::size_t rootPieceField = out.size();
    appendI32(out, 0);  // rootPiece, patched below
    appendI32(out, 0);  // collisionData, must be 0
    const std::size_t texture1Field = out.size();
    appendI32(out, 0);  // texture1, patched below
    const std::size_t texture2Field = out.size();
    appendI32(out, 0);  // texture2, patched below

    // --- Piece records, contiguous so children can be addressed -------------
    const std::size_t pieceBase = out.size();
    for (std::size_t i = 0; i < spec.pieces.size(); ++i) {
        const S3oPiece& piece = spec.pieces[i];
        appendI32(out, 0);                                             // name, patched
        appendI32(out, 0);                                             // numchildren, patched
        appendI32(out, 0);                                             // children, patched
        appendI32(out, static_cast<std::int32_t>(piece.vertices.size()));
        appendI32(out, 0);                                             // vertices, patched
        appendI32(out, 0);                                             // vertexType
        appendI32(out, piece.primitiveType);
        appendI32(out, static_cast<std::int32_t>(piece.indices.size()));
        appendI32(out, 0);                                             // vertexTable, patched
        appendI32(out, 0);                                             // collisionData
        appendF32(out, piece.xoffset);
        appendF32(out, piece.yoffset);
        appendF32(out, piece.zoffset);
    }

    const auto pieceAt = [&](std::size_t index) {
        return pieceBase + index * kPieceSize;
    };

    // --- Strings -----------------------------------------------------------
    patchI32(out, texture1Field, static_cast<std::int32_t>(out.size()));
    appendString(out, spec.texture1);
    patchI32(out, texture2Field, static_cast<std::int32_t>(out.size()));
    appendString(out, spec.texture2);

    for (std::size_t i = 0; i < spec.pieces.size(); ++i) {
        patchI32(out, pieceAt(i) + kPieceOffName, static_cast<std::int32_t>(out.size()));
        appendString(out, spec.pieces[i].name);
    }

    // --- Per-piece vertex arrays and index tables ---------------------------
    for (std::size_t i = 0; i < spec.pieces.size(); ++i) {
        const S3oPiece& piece = spec.pieces[i];

        if (!piece.vertices.empty()) {
            patchI32(out, pieceAt(i) + kPieceOffVertices, static_cast<std::int32_t>(out.size()));
            for (const S3oVertex& v : piece.vertices) {
                appendF32(out, v.px);
                appendF32(out, v.py);
                appendF32(out, v.pz);
                appendF32(out, v.nx);
                appendF32(out, v.ny);
                appendF32(out, v.nz);
                appendF32(out, v.u);
                appendF32(out, v.v);
            }
        }

        if (!piece.indices.empty()) {
            patchI32(out, pieceAt(i) + kPieceOffTable, static_cast<std::int32_t>(out.size()));
            for (const std::uint32_t index : piece.indices) {
                appendU32(out, index);
            }
        }
    }

    // --- Child tables ------------------------------------------------------
    for (std::size_t parent = 0; parent < spec.pieces.size(); ++parent) {
        std::vector<std::size_t> children;
        for (std::size_t child = 0; child < spec.pieces.size(); ++child) {
            if (spec.pieces[child].parent >= 0
                && static_cast<std::size_t>(spec.pieces[child].parent) == parent) {
                children.push_back(child);
            }
        }
        if (children.empty()) {
            continue;
        }

        patchI32(out, pieceAt(parent) + kPieceOffNumChildren,
                 static_cast<std::int32_t>(children.size()));
        patchI32(out, pieceAt(parent) + kPieceOffChildren,
                 static_cast<std::int32_t>(out.size()));

        for (const std::size_t child : children) {
            // A cycle is written by pointing the root's first child at the root.
            const std::size_t target = (spec.cyclicChild && parent == 0 && child == children.front())
                                           ? 0
                                           : child;
            appendI32(out, static_cast<std::int32_t>(pieceAt(target)));
        }
    }

    patchI32(out, rootPieceField, static_cast<std::int32_t>(pieceAt(0)));

    if (spec.vertexOffsetPastEnd && !spec.pieces.empty()) {
        patchI32(out, pieceAt(0) + kPieceOffVertices,
                 static_cast<std::int32_t>(out.size() + 4096));
    }

    // Unused but referenced by the header layout above; silences a warning about
    // the constant going unread in configurations that change the header size.
    static_assert(kHeaderSize == 52, "S3O header is 52 bytes on disk");

    return out;
}

S3oSpec twoPieceSpec() {
    S3oSpec spec;

    S3oPiece root;
    root.name = "base";
    root.parent = -1;
    root.xoffset = 1.0f;
    root.yoffset = 2.0f;
    root.zoffset = 3.0f;
    root.vertices = {
        S3oVertex{0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        S3oVertex{10.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
        S3oVertex{0.0f, 0.0f, 10.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    };
    root.indices = {0, 1, 2};

    S3oPiece turret;
    turret.name = "turret";
    turret.parent = 0;
    turret.xoffset = 10.0f;
    turret.yoffset = 20.0f;
    turret.zoffset = 30.0f;
    turret.vertices = {
        S3oVertex{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        S3oVertex{5.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        S3oVertex{0.0f, 5.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    };
    turret.indices = {0, 1, 2};

    spec.pieces = {root, turret};
    return spec;
}

} // namespace rmtest
