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
#include "core/ui/Hud.hpp"
#include "core/ui/Minimap.hpp"

#include <array>
#include <optional>
#include <vector>

namespace rm::app {

void appendMinimapPips(std::vector<rm::ui::MinimapPip>& out, const UnitScene& scene);

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
