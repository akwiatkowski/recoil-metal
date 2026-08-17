#pragma once

#include <array>
#include <cstddef>

namespace rm {

// RGBA, linear 0..1. Alpha is NOT transparency: both engines multiply it into
// the model's alpha mask, so it stays 1.
using TeamColour = std::array<float, 4>;

// Player colours, used to tint the team-colour mask both content families
// carry:
//
//   .s3o  tex1's alpha is the mask — `mix(texColor1.rgb, teamCol.rgb,
//         texColor1.a)` (ModelFragProgGL4.glsl:101).
//   .scm  the `_specTeam` texture's alpha plays the same role in Supreme
//         Commander's shaders.
//
// These are OURS, not either game's. Both assign colours at match start — BAR
// from the lobby, FA from the army table in the scenario — and neither is a
// property of a model or a map, so there is nothing to read off disk. Eight
// distinct hues at similar luminance, which is what makes teams tellable apart
// against terrain of any brightness.
inline constexpr std::array<TeamColour, 8> kTeamColours{{
    {{0.15f, 0.35f, 0.90f, 1.0f}},  // blue
    {{0.90f, 0.15f, 0.15f, 1.0f}},  // red
    {{0.20f, 0.70f, 0.25f, 1.0f}},  // green
    {{0.95f, 0.80f, 0.15f, 1.0f}},  // yellow
    {{0.65f, 0.25f, 0.80f, 1.0f}},  // purple
    {{0.20f, 0.80f, 0.85f, 1.0f}},  // cyan
    {{0.95f, 0.50f, 0.10f, 1.0f}},  // orange
    {{0.92f, 0.92f, 0.92f, 1.0f}},  // white
}};

/// Colour for a team index, wrapping — a map may declare more start positions
/// than the palette has entries, and running out is not a reason to fail.
[[nodiscard]] constexpr TeamColour teamColour(std::size_t team) noexcept {
    return kTeamColours[team % kTeamColours.size()];
}

} // namespace rm
