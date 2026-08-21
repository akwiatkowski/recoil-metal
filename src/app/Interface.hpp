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

#include "core/scene/Particles.hpp"
#include "core/scene/Picking.hpp"
#include "core/scene/UnitIcons.hpp"
#include "core/camera/OrbitCamera.hpp"
#include "core/ui/BuildPanel.hpp"
#include "core/ui/Hud.hpp"
#include "core/ui/Minimap.hpp"

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
