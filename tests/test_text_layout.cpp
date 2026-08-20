// Where each glyph goes.
//
// Pure arithmetic, and worth testing precisely because the failure mode is unreadable text —
// which is indistinguishable by eye from a font that failed to load, a pipeline that never
// ran, or a colour that came out transparent. An assertion tells them apart.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/text/TextLayout.hpp"

#include <vector>

using Catch::Approx;
using rm::text::Glyph;
using rm::text::TextVertex;

namespace {

/// A table where every glyph is a 10x20 box with an advance of 12, except space, which has an
/// advance and no area — the distinction the layout has to respect.
[[nodiscard]] std::vector<Glyph> boxFont() {
    std::vector<Glyph> glyphs(rm::text::kGlyphCount);
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        glyphs[i] = Glyph{
            .uv = {0.0f, 0.0f, 0.5f, 1.0f},
            .width = 10.0f,
            .height = 20.0f,
            .bearingX = 1.0f,
            .bearingY = -20.0f,  // sits ON the baseline, so it extends upward
            .advance = 12.0f,
        };
    }
    // Space: an advance, no box.
    glyphs[0] = Glyph{.uv = {}, .width = 0.0f, .height = 0.0f, .advance = 6.0f};
    return glyphs;
}

} // namespace

TEST_CASE("a glyph becomes two triangles at the pen") {
    const std::vector<Glyph> font = boxFont();
    std::vector<TextVertex> out;

    const float end = rm::text::appendText(out, font, "A", 100.0f, 50.0f, {1, 1, 1, 1});

    REQUIRE(out.size() == rm::text::kVerticesPerGlyph);
    CHECK(end == Approx(112.0f));  // the pen moved by the advance, not the width

    // The pen is on the BASELINE, so a letter sits above it: bearingY of -20 puts the top at
    // y = 30 and the bottom back at the baseline.
    CHECK(out[0].position[0] == Approx(101.0f));  // x + bearingX
    CHECK(out[0].position[1] == Approx(30.0f));
    CHECK(out[5].position[0] == Approx(111.0f));  // + width
    CHECK(out[5].position[1] == Approx(50.0f));   // back to the baseline
}

TEST_CASE("a space moves the pen and draws nothing") {
    // Skipping it entirely would run every word together; drawing it would put a box between
    // them. Both are wrong in ways that look like a font problem.
    const std::vector<Glyph> font = boxFont();
    std::vector<TextVertex> out;

    const float end = rm::text::appendText(out, font, " ", 0.0f, 0.0f, {1, 1, 1, 1});

    CHECK(out.empty());
    CHECK(end == Approx(6.0f));
}

TEST_CASE("a run of glyphs advances left to right and reports where it ended") {
    const std::vector<Glyph> font = boxFont();
    std::vector<TextVertex> out;

    const float end = rm::text::appendText(out, font, "AB C", 0.0f, 0.0f, {1, 1, 1, 1});

    // Three boxes, one space.
    CHECK(out.size() == 3 * rm::text::kVerticesPerGlyph);
    CHECK(end == Approx(12.0f + 12.0f + 6.0f + 12.0f));

    // Chaining: a second run starting where the first ended is what lets a HUD put a label
    // and a number in different colours on one line without measuring twice.
    const std::size_t before = out.size();
    const float second = rm::text::appendText(out, font, "D", end, 0.0f, {1, 0, 0, 1});
    CHECK(out.size() == before + rm::text::kVerticesPerGlyph);
    CHECK(second == Approx(end + 12.0f));
    CHECK(out[before].position[0] == Approx(end + 1.0f));
    CHECK(out[before].colour[1] == Approx(0.0f));  // the new colour, not the old
}

TEST_CASE("measuring and laying out agree, trailing space included") {
    // Measured from the ADVANCES rather than the widths, or a right-aligned string with a
    // trailing space lands a few pixels off and nothing says why.
    const std::vector<Glyph> font = boxFont();
    std::vector<TextVertex> out;

    for (const std::string_view text : {"", "A", "AAA", "A A", "A "}) {
        out.clear();
        const float end = rm::text::appendText(out, font, text, 0.0f, 0.0f, {1, 1, 1, 1});
        CHECK(rm::text::measureText(font, text) == Approx(end));
    }
}

TEST_CASE("scale multiplies the geometry and the advance together") {
    // Or a scaled string's letters would overlap, or gap, depending which way it went.
    const std::vector<Glyph> font = boxFont();
    std::vector<TextVertex> one;
    std::vector<TextVertex> two;

    rm::text::appendText(one, font, "AA", 0.0f, 0.0f, {1, 1, 1, 1}, 1.0f);
    rm::text::appendText(two, font, "AA", 0.0f, 0.0f, {1, 1, 1, 1}, 2.0f);

    REQUIRE(one.size() == two.size());
    CHECK(rm::text::measureText(font, "AA", 2.0f)
          == Approx(rm::text::measureText(font, "AA", 1.0f) * 2.0f));

    // The second glyph's left edge scales with everything else.
    const std::size_t second = rm::text::kVerticesPerGlyph;
    CHECK(two[second].position[0] == Approx(one[second].position[0] * 2.0f));
}

TEST_CASE("a character the atlas does not cover is dropped, not substituted") {
    // A box glyph would be inventing a character the font does not have, and a line of tofu
    // is worse than a line with a gap.
    const std::vector<Glyph> font = boxFont();
    std::vector<TextVertex> out;

    // A newline and a high byte, neither of which is printable ASCII.
    const float end = rm::text::appendText(out, font, "A\nB\x80", 0.0f, 0.0f, {1, 1, 1, 1});

    CHECK(out.size() == 2 * rm::text::kVerticesPerGlyph);  // only A and B
    CHECK(end == Approx(24.0f));
}

TEST_CASE("a degenerate scale draws nothing rather than mirroring every glyph") {
    const std::vector<Glyph> font = boxFont();
    std::vector<TextVertex> out;

    CHECK(rm::text::appendText(out, font, "A", 7.0f, 0.0f, {1, 1, 1, 1}, 0.0f) == Approx(7.0f));
    CHECK(out.empty());
    CHECK(rm::text::appendText(out, font, "A", 7.0f, 0.0f, {1, 1, 1, 1}, -2.0f) == Approx(7.0f));
    CHECK(out.empty());
    CHECK(rm::text::measureText(font, "A", -2.0f) == Approx(0.0f));
}

TEST_CASE("a short glyph table is not read past its end") {
    // The table comes from the platform's rasteriser, and a font that failed to provide every
    // glyph must not turn into a read off the end of the array.
    const std::vector<Glyph> tiny(3);
    std::vector<TextVertex> out;
    CHECK(rm::text::appendText(out, tiny, "AAA~~~", 0.0f, 0.0f, {1, 1, 1, 1}) == Approx(0.0f));
    CHECK(out.empty());
    CHECK(rm::text::measureText(tiny, "~") == Approx(0.0f));
}

TEST_CASE("uvs come straight from the glyph, so the atlas decides what is drawn") {
    std::vector<Glyph> font = boxFont();
    font[static_cast<std::size_t>('A' - rm::text::kFirstGlyph)].uv = {0.25f, 0.5f, 0.75f, 1.0f};

    std::vector<TextVertex> out;
    rm::text::appendText(out, font, "A", 0.0f, 0.0f, {1, 1, 1, 1});

    REQUIRE(out.size() == rm::text::kVerticesPerGlyph);
    CHECK(out[0].uv[0] == Approx(0.25f));
    CHECK(out[0].uv[1] == Approx(0.5f));
    CHECK(out[5].uv[0] == Approx(0.75f));
    CHECK(out[5].uv[1] == Approx(1.0f));
}
