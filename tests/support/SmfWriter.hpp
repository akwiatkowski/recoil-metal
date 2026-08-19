#pragma once

// A minimal but *faithful* SMF writer, used to build fixtures for the loader
// tests. It is modelled byte-for-byte on Recoil's own map writer,
// CBlankMapGenerator::GenerateSMF (rts/Map/Generation/BlankMapGenerator.cpp:134-232),
// which is the only SMF writer in the engine and therefore the authoritative
// statement of the layout.
//
// Why a writer instead of a checked-in .smf: game assets are never committed
// (AGENT.md rule 3), so a fixture has to be synthesised. The upside is
// known-answer testing — the test picks the raw words and the expected heights
// and the parser has to agree.
//
// The standing caveat, stated plainly: a writer and a parser built from the
// same reading of the spec can share the same misunderstanding and still agree
// with each other. These tests prove self-consistency. Only a real map proves
// correctness, which is what tests/test_real_map.cpp is for.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rmtest {

// Everything the engine validates is overridable so tests can build
// deliberately-corrupt files declaratively, without poking raw bytes.
struct SmfSpec {
    std::int32_t mapx = 128;  ///< squares along X; must be a multiple of 128
    std::int32_t mapy = 128;  ///< squares along Z

    float minHeight = 0.0f;     ///< elmos at raw word 0
    float maxHeight = 4096.0f;  ///< elmos at raw word 65536 (note: not 65535)

    /// Raw corner samples, row-major, (mapx+1) * (mapy+1) of them.
    /// Empty means "fill with zero".
    std::vector<std::uint16_t> heights;

    /// Feature type names, and the placements referring to them by index.
    ///
    /// Empty is the ordinary case and what most of these fixtures want: a map with
    /// no features is legitimate, and BAR's own maps are like that.
    std::vector<std::string> featureTypes;
    struct Feature {
        std::int32_t type = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float rotation = 0.0f;  ///< -32768..32767 for a full circle, as the format has it
    };
    std::vector<Feature> features;

    // --- Knobs for negative tests ------------------------------------------
    std::int32_t version = 1;
    std::int32_t squareSize = 8;
    std::int32_t texelPerSquare = 8;
    std::int32_t tilesize = 32;
    bool validMagic = true;
};

/// Serialises spec into a complete, valid .smf byte image.
[[nodiscard]] std::vector<std::byte> writeSmf(const SmfSpec& spec);

} // namespace rmtest
