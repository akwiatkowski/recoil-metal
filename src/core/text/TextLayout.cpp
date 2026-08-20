#include "core/text/TextLayout.hpp"

namespace rm::text {
namespace {

/// The glyph for a character, or null when it is outside the atlas or the table is short.
///
/// Dropping rather than substituting: a box glyph would be inventing a character the font
/// does not have, and a line of tofu is worse than a line with a gap.
[[nodiscard]] const Glyph* glyphFor(std::span<const Glyph> glyphs, char c) noexcept {
    if (c < kFirstGlyph || c > kLastGlyph) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(c - kFirstGlyph);
    return index < glyphs.size() ? &glyphs[index] : nullptr;
}

} // namespace

float appendText(std::vector<TextVertex>& out, std::span<const Glyph> glyphs,
                 std::string_view text, float x, float y, std::array<float, 4> colour,
                 float scale) {
    if (scale <= 0.0f) {
        return x;  // a zero or negative scale is not text, and would mirror every glyph
    }

    out.reserve(out.size() + text.size() * kVerticesPerGlyph);

    float pen = x;
    for (const char c : text) {
        const Glyph* glyph = glyphFor(glyphs, c);
        if (glyph == nullptr) {
            continue;
        }

        // A glyph with no area still ADVANCES the pen. A space is exactly this, and skipping
        // it entirely would run every word together.
        if (glyph->width > 0.0f && glyph->height > 0.0f) {
            const float left = pen + glyph->bearingX * scale;
            const float top = y + glyph->bearingY * scale;
            const float right = left + glyph->width * scale;
            const float bottom = top + glyph->height * scale;

            const float u0 = glyph->uv[0];
            const float v0 = glyph->uv[1];
            const float u1 = glyph->uv[2];
            const float v1 = glyph->uv[3];

            const TextVertex topLeft{.position = {left, top}, .uv = {u0, v0}, .colour = colour};
            const TextVertex topRight{
                .position = {right, top}, .uv = {u1, v0}, .colour = colour};
            const TextVertex bottomLeft{
                .position = {left, bottom}, .uv = {u0, v1}, .colour = colour};
            const TextVertex bottomRight{
                .position = {right, bottom}, .uv = {u1, v1}, .colour = colour};

            // Two triangles, wound the same way as every other quad here.
            out.push_back(topLeft);
            out.push_back(topRight);
            out.push_back(bottomLeft);
            out.push_back(bottomLeft);
            out.push_back(topRight);
            out.push_back(bottomRight);
        }

        pen += glyph->advance * scale;
    }

    return pen;
}

float measureText(std::span<const Glyph> glyphs, std::string_view text, float scale) {
    if (scale <= 0.0f) {
        return 0.0f;
    }
    float width = 0.0f;
    for (const char c : text) {
        if (const Glyph* glyph = glyphFor(glyphs, c)) {
            width += glyph->advance * scale;
        }
    }
    return width;
}

} // namespace rm::text
