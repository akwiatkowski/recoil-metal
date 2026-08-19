#pragma once

#include "core/Error.hpp"

#include <expected>
#include <filesystem>
#include <string_view>

namespace rm::prop {

// What a prop's blueprint says about drawing it.
//
// A `.scmap` places props by naming a BLUEPRINT — `/env/Tropical/Props/Trees/
// Palm02_s1_prop.bp` — not a mesh. The blueprint is Lua in data-constructor
// syntax (`PropBlueprint { ... }`, the same shape `_save.lua` uses) and carries
// the two things the placement does not: how big the prop is, and what to paint
// it with.
//
// Everything else in a blueprint is for the game rather than the renderer: audio
// cues, reclaim economy, health, collision size, the Lua script class. Read and
// ignored.
struct Blueprint {
    /// The mesh, as an absolute path. Never empty on success.
    std::filesystem::path mesh;

    /// The albedo texture, absolute. May be empty: a blueprint can name none,
    /// and the renderer has a defined fallback for that.
    std::filesystem::path albedo;

    /// The scale the mesh is authored to need.
    ///
    /// This is the field that makes reading the blueprint unavoidable rather
    /// than a nicety. Every one of the 418 942 props across the stock corpus
    /// stores a per-instance scale of exactly 1.0, so the map file says nothing
    /// about size at all — the real figure is here, and it is not close to 1:
    /// a palm's is 0.04. Skip this and every tree renders twenty-five times too
    /// big, which reads as a units bug in the mesh loader.
    float uniformScale = 1.0f;
};

/// Reads a blueprint, resolving its mesh and texture against the extracted game
/// content at `root`.
///
/// `gameRelativePath` is the path as a `.scmap` states it: absolute inside the
/// game's virtual filesystem, so it begins with a slash that has to go before
/// joining, or the result resolves to the real filesystem root.
///
/// The MESH IS NOT NAMED IN THE FILE. A blueprint's LOD table gives the textures
/// and the cutoff distances but never the geometry; the engine finds it by
/// convention, `X_prop.bp` beside `X_lod0.scm`. That resolves 199 of the 207
/// blueprints the stock maps name. The other eight are particle emitters —
/// lava steam, blowing sand, underwater bubbles, water mist — which have no mesh
/// because they are not geometry, and they come back as a MissingMesh error
/// rather than as a failure to parse.
[[nodiscard]] std::expected<Blueprint, MapError> loadFile(
    const std::filesystem::path& root, std::string_view gameRelativePath);

} // namespace rm::prop
