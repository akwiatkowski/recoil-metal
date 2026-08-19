#pragma once

#include "core/Error.hpp"

#include <expected>
#include <limits>
#include <filesystem>
#include <string_view>
#include <vector>

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
// One detail level of a prop: the mesh to draw, what to paint it with, and how far
// out it is the right level to use.
struct BlueprintLod {
    std::filesystem::path mesh;    ///< absolute; never empty
    std::filesystem::path albedo;  ///< absolute, or empty when none resolved

    /// The tangent-space normal map, absolute, or empty.
    ///
    /// NOT the convention the stratum maps use, and measuring that was the whole of
    /// the work here. All 221 prop normal maps in the extracted content are BC3 with
    /// red, green and blue EQUAL — one value replicated across the colour channels
    /// and a second in alpha, means of 131 and 127 across the corpus. So they carry
    /// two axes, not three: the third is reconstructed. A stratum map by contrast
    /// puts z in blue near 255 (ADR-020), and reading one of these as though it were
    /// one of those lights every prop from a direction the artist never chose.
    ///
    /// Two channels because BC3's alpha block is a better encoder than its RGB565
    /// colour block, so an axis kept in alpha survives compression where a third of
    /// a packed RGB triple does not.
    std::filesystem::path normals;

    float cutoffElmos = std::numeric_limits<float>::infinity();
};

// An ambient effect a prop marks the place for, rather than a thing to draw.
//
// Eight of the blueprints the stock maps reference draw nothing: `UniformScale = 0`
// and a `MeshName` pointing at an editor marker. What they mark is a particle
// effect, and the blueprint says WHICH — "Water surface mist", "Small lava steam
// steam", "Underwater bubbles", "Desert Blowing Sand" — but nothing whatever about
// how it looks. That lives in the Lua the engine runs, which this project does not
// have and by settled decision will not port.
//
// So the KIND is read and the appearance is ours, which is the arrangement ADR-018
// already made for the water and the sky: port the structure, name the stand-ins.
enum class Effect {
    None,
    Steam,        ///< lava steam, rising and thinning
    Mist,         ///< water-surface mist, low and slow
    Bubbles,      ///< underwater, rising fast and small
    BlowingSand,  ///< desert, drifting sideways
    BlowingSnow,  ///< tundra, the same but paler and slower
};

struct Blueprint {
    /// The ambient effect this prop marks, or None when it is geometry.
    ///
    /// EXACTLY ONE of this and `lods` is meaningful: a blueprint either draws a mesh
    /// or marks an effect. A scale of zero is what says which — the structural fact
    /// — and the file's name is what says which effect, since nothing else in it
    /// does.
    Effect effect = Effect::None;

    /// The levels, FINEST FIRST, non-empty unless `effect` says otherwise.
    ///
    /// Most props have one. The ones that matter have three: every heavily-placed
    /// tree in the corpus declares cutoffs like 30 / 175 / 300 ogrids, which is
    /// where the saving is, because those are the blueprints placed ten thousand
    /// times. Boulders and the like declare a single level and a near cutoff, so
    /// they are culled outright before detail would have mattered.
    ///
    /// The LOD TABLE is the authority on how many levels there are, not the files
    /// on disk: 14 blueprints ship four meshes while declaring fewer entries, and
    /// drawing a level the blueprint does not describe would be inventing content.
    std::vector<BlueprintLod> lods;

    /// The scale the mesh is authored to need.
    ///
    /// This is the field that makes reading the blueprint unavoidable rather
    /// than a nicety. Every one of the 418 942 props across the stock corpus
    /// stores a per-instance scale of exactly 1.0, so the map file says nothing
    /// about size at all — the real figure is here, and it is not close to 1:
    /// a palm's is 0.04. Skip this and every tree renders twenty-five times too
    /// big, which reads as a units bug in the mesh loader.
    float uniformScale = 1.0f;

    /// Beyond this distance the prop is not drawn at all: the coarsest level's
    /// cutoff, which is the same thing said from the other end.
    [[nodiscard]] float drawDistanceElmos() const noexcept {
        return lods.empty() ? 0.0f : lods.back().cutoffElmos;
    }
};

/// Reads a blueprint, resolving its mesh and texture against the extracted game
/// content at `root`.
///
/// `gameRelativePath` is the path as a `.scmap` states it: absolute inside the
/// game's virtual filesystem, so it begins with a slash that has to go before
/// joining, or the result resolves to the real filesystem root.
///
/// THE MESHES ARE NOT NAMED IN THE FILE. A blueprint's LOD table gives the textures
/// and the cutoff distances but never the geometry; the engine finds it by
/// convention, `X_prop.bp` beside `X_lod0.scm`, `X_lod1.scm` and so on, one per
/// entry in the table. That resolves 199 of the 207 blueprints the stock maps name.
/// The other eight are particle emitters — lava steam, blowing sand, underwater
/// bubbles, water mist — which have no mesh because they are not geometry, and they
/// come back as a MissingMesh error rather than as a failure to parse.
///
/// A level whose mesh is missing ends the list rather than failing the blueprint: a
/// prop with a finer level and no coarser one is still a prop.
[[nodiscard]] std::expected<Blueprint, MapError> loadFile(
    const std::filesystem::path& root, std::string_view gameRelativePath);

} // namespace rm::prop
