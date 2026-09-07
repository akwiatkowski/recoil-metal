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
#include "core/scene/ProjectileFx.hpp"
#include "core/scene/Particles.hpp"
#include "core/scene/Picking.hpp"
#include "core/scene/UnitIcons.hpp"
#include "core/camera/OrbitCamera.hpp"
#include "core/ui/BuildPanel.hpp"
#include "core/ui/GameProfile.hpp"
#include "core/ui/Hud.hpp"
#include "core/ui/Minimap.hpp"
#include "core/ui/ProductionPanel.hpp"
#include "core/ui/Roster.hpp"

#include <array>
#include <span>
#include <utility>
#include <string>
#include <optional>
#include <vector>

namespace rm::app {

/// Queues the factory control under a HUD point through the normal semantic command path.
/// Dispatch still validates ownership; the view and button state are never mutated directly.
[[nodiscard]] bool submitProductionControl(UnitScene& scene, rm::sim::UnitId builder,
    rm::PlayerIndex player, rm::TickIndex tick, const rm::ui::FrameLayout& frame,
    float x, float y, std::size_t page = 0);

/// The selected-unit outline for the active game presentation.
void appendUnitSelection(std::vector<rm::DecalVertex>& out, const rm::HeightField& field,
                         std::array<float, 3> centre, float radius, rm::ui::GameProfile profile);

/// Public map resources: green mass rings and amber hydrocarbon rings.
void appendResourceDeposits(std::vector<rm::DecalVertex>& out, const UnitScene& scene,
                            const rm::HeightField& field);
[[nodiscard]] std::array<float, 2> snapResourceSite(const UnitScene& scene,
    rm::UnitTypeIndex type, std::array<float, 2> at);

/// Where a structure order at `at` will actually stand: the deposit centre for deposit-bound
/// structures, otherwise the build grid point in Grid mode or `at` itself in Free mode. The
/// same rule the sim applies at command intake, so the ghost never disagrees with the order.
[[nodiscard]] std::array<float, 2> snapBuildSite(const UnitScene& scene, rm::UnitTypeIndex type,
                                                 std::array<float, 2> at);

void appendMinimapPips(std::vector<rm::ui::MinimapPip>& out, const UnitScene& scene);

/// Which unit the panel is showing the options OF.
///
/// THE HANDLE AND THE NAME TOGETHER, because the panel header and command dispatch must identify
/// the same exact unit. `gatherBuildOptions` accepts that active handle explicitly and returns it
/// here, so a caller never has to re-derive who owns the menu it is drawing.
struct BuildSelection {
    rm::sim::UnitId builder{};
    std::string name;

    /// The builder's role, as `roleName` spells it — "commander", "builder". The panel header
    /// leads with this and demotes the blueprint id to a suffix, because a role is the word a
    /// player thinks in and an id is how they name the unit to somebody else.
    std::string role;

    [[nodiscard]] bool any() const noexcept { return !name.empty(); }
};

/// Submit the same immediate build-cell action used by the window and headless UI tests.
[[nodiscard]] bool submitBuildOption(UnitScene& scene, const rm::vfs::Vfs& content,
    rm::sim::UnitId builder, rm::PlayerIndex player, rm::TickIndex tick,
    const rm::ui::BuildOption& option, bool shift = false);

/// Follow completed upgrades while preserving selection membership.
void followUpgradeSelection(const UnitScene& scene, std::span<rm::sim::UnitId> selection);

/// Build-capable handles in selection order.
///
/// Dead, unknown and non-building units are omitted. Output is cleared on every call so a caller
/// can keep the vector across frames without stale handles surviving a deselection.
void gatherBuilderCandidates(const UnitScene& scene,
                             std::span<const rm::sim::UnitId> selection,
                             std::vector<rm::sim::UnitId>& out);

/// Retains `current` while it remains a candidate; otherwise chooses the first candidate.
///
/// Selection order is already deterministic, so the fallback is deterministic without sorting
/// handles and changing the player's selection order. No candidate returns an invalid handle.
[[nodiscard]] rm::sim::UnitId
activeBuilderFor(std::span<const rm::sim::UnitId> candidates,
                 rm::sim::UnitId current = {}) noexcept;

/// What one explicit active builder can build, for the build panel.
///
/// Choosing one builder rather than intersecting every selected builder is deliberate. Both games
/// this engine reads from do it that way, and an intersection would empty the panel whenever a
/// tank is box-selected with a commander. Candidate choice lives outside this translation so the
/// same builder can remain active when selection order changes.
///
/// Returns the builder through `who` so the header can say whose list this is and a placement
/// can be ordered from it. Invalid input clears both outputs; a valid builder can still have no
/// offered options, in which case the panel is absent rather than empty.
///
/// ANSWERED FROM `scene.roster`, NOT FROM `BuildTree` OVER `scene.definitions` — the reason is
/// in the implementation, and it is the difference between "every structure this faction
/// fields" and "the one structure that happens to have been registered so far".
///
/// `out` and `who` are cleared on every call, including the ones that find nothing, so a caller
/// may reuse both across frames without a deselection leaving the last builder's menu on screen.
void gatherBuildOptions(const UnitScene& scene, rm::sim::UnitId activeBuilder,
                         const rm::ui::Theme& theme, std::vector<rm::ui::BuildOption>& out,
                         BuildSelection& who);

/// Every visible icon the interface needs this frame, packed into ONE atlas.
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
/// Clears every `iconSlot`, then assigns slots only inside the two visible ranges. A catalog can
/// therefore contain hundreds of entries without pushing strategic glyphs or classic chrome out
/// of the fixed atlas; changing page is one of the caller's repack keys.
///
/// Rebuilt when the SET changes, not per frame. The caller decides that; this just does the
/// work, and it is a memcpy per icon rather than a decode (`core/ui/IconAtlas.hpp`).
/// `strategic` is the scene's cached strategic glyph art, appended after the tray's and the
/// roster's icons; `strategicBase` comes back as the slot its first entry landed in, which
/// with the cache's own indices is the whole mapping `buildStrategicIconRefs` needs.
struct PackedInterfaceAtlas {
    rm::dds::Texture texture;
    rm::ui::PanelSkin skin;
};

struct VisibleIconRange {
    std::size_t first = 0;
    std::size_t count = 0;
};

[[nodiscard]] PackedInterfaceAtlas packInterfaceIcons(
    const rm::vfs::Vfs& content, std::vector<rm::ui::BuildOption>& options,
    std::vector<rm::ui::RosterTile>& tiles,
    VisibleIconRange visibleBuild, VisibleIconRange visibleRoster, rm::ui::GameProfile profile,
    std::span<const std::pair<std::string, rm::dds::Texture>> strategic = {},
    std::size_t* strategicBase = nullptr);

/// What the selection is made of, grouped by type, for the roster.
///
/// FROM THE STORE rather than from the draw gather, so a unit that is selected but off screen
/// still counts — a roster is a statement about the selection, not about what is visible.
/// Dead handles are skipped, which is how a selection outlives the units in it.
void gatherRoster(const UnitScene& scene, std::span<const rm::sim::UnitId> selection,
                  std::vector<rm::ui::RosterTile>& out);

[[nodiscard]] rm::ui::InfoCard selectedUnitCard(const UnitScene& scene,
    const rm::ui::RosterTile& tile, rm::sim::UnitId activeBuilder);

/// What the active builder is producing, when it is a factory: its Build orders in queue
/// order with their counts, the progress of the construction it is running, and its repeat
/// state. Nothing for a mobile builder, a dead handle, or a unit that cannot build — an
/// engineer's queue is drawn in the world, not in the deck.
[[nodiscard]] std::optional<rm::ui::ProductionView> gatherProduction(const UnitScene& scene,
                                                                     rm::sim::UnitId builder);

void appendViewFootprint(std::vector<std::array<float, 2>>& out, const rm::OrbitCamera& camera,
                         const rm::HeightField& field, const rm::ui::UiViewport& viewport);

[[nodiscard]] rm::ui::MatchState hudStateFrom(
    const UnitScene& scene, float elapsedSeconds, rm::ui::GameProfile profile);

/// Resolves the run's explicit profile against the player's faction and an optional packed skin.
/// BAR and Neutral own concrete materials; incomplete classic art falls back atomically to Neutral.
[[nodiscard]] rm::ui::Theme hudThemeFor(const UnitScene& scene, rm::ui::GameProfile profile,
                                         const rm::ui::PanelSkin& skin = {});

/// One type's strategic icon, as the atlas holds it this pack: which slot, and the glyph's
/// own texel size — the icons ship at mixed small sizes (16x16-ish) and are packed into a
/// cell's corner, so the size is what turns a slot into a uv rectangle and a quad.
struct StrategicIconRef {
    std::size_t slot = 0;
    int width = 0;
    int height = 0;
};

/// Loads any strategic glyph a REGISTERED type names that is not yet cached — a few archive
/// reads the first time a type appears, and none thereafter. Lazy by registration rather
/// than eager over the corpus, so a match only ever pays for the icons it can show; failures
/// are remembered too, or a missing file would be re-read every pack.
void ensureStrategicIconArt(UnitScene& scene, const rm::vfs::Vfs& content);

/// The per-type icon table for THIS pack: `out[type]` is the type's glyph at `base + its
/// index in the art cache`, or nothing — nothing meaning the plain square fallback. Rebuilt
/// after every pack, because the base moves with the tray's and roster's counts.
void buildStrategicIconRefs(const UnitScene& scene, std::size_t base,
                            std::vector<std::optional<StrategicIconRef>>& out);

/// The strategic layer: a glyph, in the army's colour, for every drawn unit whose type has
/// one and whose mesh has shrunk past reading. Into `out.worldOverlay.image` — under the HUD,
/// where a picture of the battlefield belongs. Units without a glyph are left to
/// `appendSceneIcons`' squares, which is the honest fallback for the 18 nameless blueprints
/// and every BAR unit.
void appendStrategicIcons(rm::ui::Geometry& out, const UnitScene& scene,
                          const rm::OrbitCamera& camera, const rm::ui::UiViewport& viewport,
                          std::span<const std::optional<StrategicIconRef>> refs);

/// Whether a row in `scene.building` is work still going on, and how far along it is.
///
/// TWO RULES THAT BOTH COST A BUG. `scene.building` is a LEDGER, not a queue: the sim reports
/// what newly completed and removes nothing, because the census counts a standing extractor by
/// finding its completed row (`Match.cpp`'s `standingFor`). Drawing every row put a permanent
/// half-built ghost on top of every building an army had ever finished. And a blueprint that
/// states no build time has a total of zero, which reads as "complete" to any division and as
/// "nothing to draw" to a renderer — the honest answer is that it has barely started, since the
/// work still runs on the economy's own schedule.
[[nodiscard]] bool constructionInProgress(const rm::sim::Construction& work) noexcept;
[[nodiscard]] float constructionProgress(const rm::sim::Construction& work) noexcept;
/// Active work owned by the selected builder (including self-upgrades), for the inspector.
[[nodiscard]] std::optional<rm::ui::InfoCard> constructionCard(
    const UnitScene& scene, rm::sim::UnitId builder);

/// Every construction the player can see, as something to draw at its site.
///
/// THE MISSING BODY. `sim::Construction` is an economic row and nothing more — the sim spawns
/// no unit until the work completes — so between ordering a building and its arrival the site
/// was bare ground. This is the translation step that gives it one: the product's model, its
/// progress, and the builder's faction, which is what decides the effect.
///
/// RESOLVES THE PRODUCT'S MODEL ON DEMAND, through `ensureDrawableType`: a construction is the
/// first time in a match that a blueprint needs to be DRAWN, and until now that only happened
/// when the finished unit spawned. The batch list may therefore grow inside this call, which is
/// why the caller must upload before drawing (see the frame loop's `uploadNewBatches`).
///
/// Fogged sites are left out — a construction is a building the enemy has not seen yet, and
/// drawing one through the fog would be a free scouting report.
void gatherConstructions(UnitScene& scene, const rm::vfs::Vfs& content,
                         const rm::HeightField& field,
                         std::vector<rm::Renderer::ConstructionDraw>& out);

/// The ground and air effects that go with them: a glowing pad under each site, and construction
/// beams from every builder that worked on one this tick. UEF uses its authored build bone and
/// paired descending cube-edge sweep; other factions retain the centre stream for now.
///
/// SEPARATE FROM THE MODEL PASS because they ride existing machinery — the pad is a ground
/// decal like a selection ring, the stream is particles like dust — and adding a third
/// pipeline for a line of light would be a pipeline to maintain for one effect.
void appendConstructionEffects(std::vector<rm::DecalVertex>& decals,
                               std::vector<rm::Particle>& particles, const UnitScene& scene,
                               const rm::HeightField& field, float seconds);

/// Anonymous world-space crosses at radar/sonar-reported positions. They deliberately use
/// only `Contact::x/z`: no type, owner, health, or true position crosses the fog boundary.
void appendContactBlips(rm::ui::Geometry& out, const UnitScene& scene,
                         const rm::OrbitCamera& camera, const rm::HeightField& field,
                         const rm::text::Font& font, const rm::ui::UiViewport& viewport);

/// `refs`, when given, names the types the strategic layer already drew — their units are
/// skipped here rather than drawn twice, once as artwork and once as a square.
void appendSceneIcons(std::vector<rm::Particle>& into, const UnitScene& scene,
                      const rm::OrbitCamera& camera,
                      std::span<const std::optional<StrategicIconRef>> refs = {});

/// Health bars over the units that need one: DAMAGED, and big enough on screen to be a unit
/// rather than an icon.
///
/// Both gates are the design. A full-health bar is noise repeated per unit — the absence of
/// a bar is what "fine" looks like, and then a bar appearing IS the signal. And below icon
/// zoom a three-point bar floats over a two-point unit, so the bar keeps to the range where
/// the unit it belongs to is legible; at height, the fight reads through the icons.
///
/// Screen-space quads into the HUD's own geometry — the same worldToScreen the band select
/// uses, drawn by the pipeline every panel already rides.
void appendHealthBars(rm::ui::Geometry& out, const UnitScene& scene,
                      const rm::OrbitCamera& camera, const rm::text::Font& font,
                      const rm::ui::UiViewport& viewport);

/// Visible work belonging to selected builders, or a site under the HUD cursor.
/// Returns the number drawn so offscreen acceptance can assert the same overlay.
std::size_t appendConstructionBars(rm::ui::Geometry& out, const UnitScene& scene,
    const rm::OrbitCamera& camera, const rm::HeightField& field, const rm::text::Font& font,
    const rm::ui::UiViewport& viewport, std::span<const rm::sim::UnitId> selected,
    std::optional<std::array<float, 2>> cursor = std::nullopt);

[[nodiscard]] std::optional<rm::sim::UnitId> pickAnyBatch(const rm::Ray& ray,
                                                          const UnitScene& scene);

[[nodiscard]] bool hostileTo(const UnitScene& scene, int army, rm::sim::UnitId id);
[[nodiscard]] bool alliedTo(const UnitScene& scene, int army, rm::sim::UnitId id);

[[nodiscard]] std::optional<rm::sim::UnitId> pickAcrossBatches(const rm::Ray& ray,
                                                              const UnitScene& scene);

} // namespace rm::app
