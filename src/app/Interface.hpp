#pragma once

// The interface, and the picking behind it: what the player sees and what the player clicks.
//
// EXTRACTED FROM `main.mm` FOR PLAN2.md §7 P7.5. The top layer: everything here reads a scene
// and produces something for the renderer or answers a question about a click. Nothing here is
// read back into the sim, which is the same rule `core/scene/UnitDraw.hpp` states from the other
// side of the seam.
//
// WHY THE PICKING IS HERE AND NOT IN `core/scene/Picking.hpp`. That file answers "which
// instance does this ray hit" over a batch — geometry, and testable without a scene. These two
// answer "which UNIT", which needs the batch-to-slot mapping the draw gather built and the army
// filter selection applies. They are scene questions wearing a geometry hat.

#include "app/Match.hpp"

#include "core/scene/CombatEffects.hpp"
#include "core/scene/Particles.hpp"
#include "core/scene/Picking.hpp"
#include "core/scene/UnitIcons.hpp"
#include "core/camera/OrbitCamera.hpp"
#include "core/ui/BuildPanel.hpp"
#include "core/ui/Hud.hpp"
#include "core/ui/Minimap.hpp"
#include "core/ui/Roster.hpp"

#include <array>
#include <span>
#include <string>
#include <optional>
#include <vector>

namespace rm::app {

void appendMinimapPips(std::vector<rm::ui::MinimapPip>& out, const UnitScene& scene);

/// Which unit the panel is showing the options OF.
///
/// THE HANDLE AND THE NAME TOGETHER, because both callers need one each and re-deriving either
/// means restating "the first builder in the selection decides" somewhere else. That rule lives
/// in `gatherBuildOptions` and a second copy of it is a copy that will drift — the panel would
/// then be headed by one unit and its orders issued by another, which is invisible until the
/// day two builders are selected.
struct BuildSelection {
    rm::sim::UnitId builder{};
    std::string name;

    /// The builder's role, as `roleName` spells it — "commander", "builder". The panel header
    /// leads with this and demotes the blueprint id to a suffix, because a role is the word a
    /// player thinks in and an id is how they name the unit to somebody else.
    std::string role;

    [[nodiscard]] bool any() const noexcept { return !name.empty(); }
};

/// What the selection can build, for the build panel.
///
/// THE FIRST BUILDER IN THE SELECTION DECIDES, rather than the intersection of everything
/// selected. Both games this engine reads from do it that way, and the reason is that an
/// intersection empties the panel the moment a tank is box-selected along with a commander —
/// which is the common case, and a panel that empties when you select MORE is the sort of
/// behaviour a player learns to work around instead of using.
///
/// Returns the builder through `who` so the header can say whose list this is and a placement
/// can be ordered from it; empty output means nothing selected builds anything, and the panel is
/// then absent rather than empty.
///
/// ANSWERED FROM `scene.roster`, NOT FROM `BuildTree` OVER `scene.definitions` — the reason is
/// in the implementation, and it is the difference between "every structure this faction
/// fields" and "the one structure that happens to have been registered so far".
///
/// `out` and `who` are cleared on every call, including the ones that find nothing, so a caller
/// may reuse both across frames without a deselection leaving the last builder's menu on screen.
void gatherBuildOptions(const UnitScene& scene, std::span<const rm::sim::UnitId> selection,
                        const rm::ui::Theme& theme, std::vector<rm::ui::BuildOption>& out,
                        BuildSelection& who);

/// Every icon the interface needs this frame, packed into ONE atlas.
///
/// BOTH PANELS AT ONCE, and that is forced rather than chosen: the renderer binds one icon
/// texture, so a build tray and a roster with separate atlases would be two binds and two
/// draws — or, worse, one atlas silently drawn with the other's slots, which is how a cell ends
/// up showing a unit that is not in it. Packing them together makes the slots one numbering by
/// construction.
///
/// FROM THE ARCHIVES, at `textures/ui/common/icons/units/<ID>_icon.dds` — 538 of them ship in
/// `textures.scd`. A blueprint with no icon there is ordinary (`UEB5208` is one) and keeps its
/// slot blank rather than borrowing its neighbour's: `packIcons` is positional, so the slot a
/// cell reads is the slot its own icon went into whether or not that icon existed.
///
/// ASSIGNS `iconSlot` on every option and tile as a side effect, which is the whole point — the
/// atlas and the indices into it are one answer, and separating them would let a caller pair
/// last frame's slots with this frame's atlas.
///
/// Rebuilt when the SET changes, not per frame. The caller decides that; this just does the
/// work, and it is a memcpy per icon rather than a decode (`core/ui/IconAtlas.hpp`).
[[nodiscard]] rm::dds::Texture packInterfaceIcons(const rm::vfs::Vfs& content,
                                                  std::vector<rm::ui::BuildOption>& options,
                                                  std::vector<rm::ui::RosterTile>& tiles);

/// What the selection is made of, grouped by type, for the roster.
///
/// FROM THE STORE rather than from the draw gather, so a unit that is selected but off screen
/// still counts — a roster is a statement about the selection, not about what is visible.
/// Dead handles are skipped, which is how a selection outlives the units in it.
void gatherRoster(const UnitScene& scene, std::span<const rm::sim::UnitId> selection,
                  std::vector<rm::ui::RosterTile>& out);

void appendViewFootprint(std::vector<std::array<float, 2>>& out, const rm::OrbitCamera& camera,
                         const rm::HeightField& field, float width, float height);

[[nodiscard]] rm::ui::MatchState hudStateFrom(const UnitScene& scene, float elapsedSeconds);

[[nodiscard]] rm::ui::Theme hudThemeFor(const UnitScene& scene);

void appendSceneIcons(std::vector<rm::Particle>& into, const UnitScene& scene,
                      const rm::OrbitCamera& camera);

[[nodiscard]] std::optional<rm::sim::UnitId> pickAnyBatch(const rm::Ray& ray,
                                                          const UnitScene& scene);

[[nodiscard]] bool hostileTo(const UnitScene& scene, int army, rm::sim::UnitId id);

[[nodiscard]] std::optional<rm::sim::UnitId> pickAcrossBatches(const rm::Ray& ray,
                                                              const UnitScene& scene);

} // namespace rm::app
