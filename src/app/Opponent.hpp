#pragma once

// The opponent port (ADR-038): an opponent observes, thinks, and hands back decisions.
//
// WHY THIS EXISTS. The scripted opponent was called by name from `runOpponents`, which made a
// second opponent a second call site and left the first one impossible to switch off, swap, or
// benchmark against a replacement. That is fine while there is exactly one; it is the thing in
// the way the moment there are two, and ADR-039 commits to a second — an adapter hosting FAF's
// own AI. This is the seam that one plugs into, sized so that adding it changes no sim code.
//
// THE SHAPE, and the one property that keeps it small: **`advance` RETURNS.** What happens
// inside is the implementation's business — the scripted opponent is a straight-line function,
// while a FAF adapter will resume a set of Lua coroutines — and the port sees only "this tick's
// work is done". Hosting coroutines is then a detail of one implementation rather than
// something every caller has to understand.
//
// WHAT AN OPPONENT MAY NOT DO: hold a pointer into the sim between calls, or mutate anything.
// `BuildOrder.hpp` already stated that rule for the script in a comment ("the script holds no
// pointers into the sim"); this promotes it to the type. `World` hands out const references and
// nothing else, and every decision comes back as data the caller applies.
//
// WHY `Decision` AND NOT `Command`. An earlier draft of ADR-038 said an opponent emits commands
// and nothing else. The code disagrees: construction does NOT go through `applyCommand` —
// `runOpponents` pushes a `Construction` into `scene.building` directly, which `Match.cpp` calls
// "a known hole rather than a preference", because `Construction::blueprintIndex` indexes
// `scene.buildable` while `applyCommand` reads `catalog.def()`. Two index spaces wearing one
// type name. So the port carries a type wide enough for what an opponent actually decides, of
// which a move order is one arm and "start this construction" is the other. Narrowing to
// `Command` means closing that index-space hole first, and is not this change's job.
//
// WHY THIS LIVES IN `app/` AND NOT `core/`. Everything an opponent reads — the unit scene, the
// buildable list, the passability grids — is owned by the app layer, and a header in `core/`
// that reached up here would invert the dependency `tools/check_sim_boundary.sh` exists to
// protect. It moves down when the data does, and not before.

#include "app/Scene.hpp"

#include "core/Types.hpp"
#include "core/map/HeightField.hpp"
#include "core/map/MapInfo.hpp"
#include "core/map/ScenarioSave.hpp"
#include "core/sim/BuildOrder.hpp"
#include "core/sim/Events.hpp"
#include "core/vfs/Vfs.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rm::ai {

/// Everything an opponent is allowed to look at, and nothing more.
///
/// A bundle of const references rather than a class with accessors: the members ARE the
/// vocabulary, and wrapping each one in a getter would add a layer that answers the same
/// questions in more words. What makes it a read view is the `const`, not the shape.
///
/// Every method a future adapter needs is a claim about what an opponent is allowed to know, so
/// this struct growing is the signal to check that claim rather than a routine edit.
struct World {
    const rm::app::UnitScene& scene;
    const rm::vfs::Vfs& content;
    const rm::HeightField& field;
    std::span<const rm::mapinfo::StartPosition> starts;
    std::span<const rm::scenario::Marker> markers;

    /// A value copy keeps an opponent that retains its observation from retaining a borrowed
    /// match-configuration address. An empty optional means automatic acquisition is unrestricted.
    std::optional<rm::sim::PlayableRect> playableRect;

    /// The middle of the map, in fixed point — where `structureSite` and `rolloffPoint` aim.
    /// Passed rather than derived so an opponent never does map arithmetic of its own.
    rm::sim::Fx centreX{};
    rm::sim::Fx centreZ{};
};

/// One thing an opponent decided to do this pass.
///
/// A TAGGED STRUCT rather than a variant or a class hierarchy, following ADR-034's ruling for
/// projectiles: the arms differ in a few fields, not in behaviour, and a `kind` a switch reads
/// is cheaper to follow than a type the reader has to chase. The unused arm's fields cost a few
/// bytes on a vector that holds a handful of entries once a second.
struct Decision {
    enum class Kind : std::uint8_t {
        /// Start building `blueprint` at `site`, paid for by `builder` at `buildRate`.
        StartConstruction,
        /// Send `unit` to (`toX`, `toZ`).
        Move,
    };

    Kind kind = Kind::Move;

    /// StartConstruction. The blueprint is a PATH, not a resolved index: resolving it needs the
    /// VFS and the scene's buildable list, which is the caller's side of the fence.
    std::string blueprint;
    std::array<rm::sim::Fx, 3> site{};
    rm::sim::UnitId builder{};
    float buildRate = 0.0f;

    /// Move.
    rm::sim::UnitId unit{};
    rm::sim::Fx toX{};
    rm::sim::Fx toZ{};
};

/// An opponent: something that plays one army.
class Opponent {
public:
    Opponent() = default;
    virtual ~Opponent() = default;
    Opponent(const Opponent&) = delete;
    Opponent& operator=(const Opponent&) = delete;
    Opponent(Opponent&&) = delete;
    Opponent& operator=(Opponent&&) = delete;

    /// Look at the world as it is, and at what happened since the last look.
    virtual void observe(const World& world, std::span<const rm::sim::Event> events) = 0;

    /// Do this tick's thinking. Returns when the thinking is done — see the header note.
    virtual void advance(rm::TickIndex tick) = 0;

    /// What it decided, valid until the next `advance`.
    [[nodiscard]] virtual std::span<const Decision> drain() const = 0;

    /// Whether this opponent has committed to its attack, which decides where a unit rolling
    /// off the factory goes: to the fight, or to the rally point.
    ///
    /// ponytail: this is the one member of the port that is NOT general — it is the scripted
    /// opponent's own notion, and a FAF adapter has no equivalent to answer with. The general
    /// version is "where should this fresh unit go", which the opponent should answer as a
    /// Decision when the spawn path stops asking the question itself. Left as-is because
    /// generalising it now would design that path blind, and the default keeps any other
    /// implementation honest by making it say no.
    [[nodiscard]] virtual bool attackLaunched() const { return false; }
};

/// Milestone 20's "not an AI", behind the port: a fixed build order plus one attack wave.
///
/// The decisions themselves stay in `core/sim/BuildOrder.hpp`, where they are pure and tested.
/// This turns them into `Decision`s against a `World`, and remembers the single bit the script
/// keeps between ticks.
class ScriptedOpponent final : public Opponent {
public:
    void observe(const World& world, std::span<const rm::sim::Event> events) override;
    void advance(rm::TickIndex tick) override;
    [[nodiscard]] std::span<const Decision> drain() const override { return decisions_; }
    [[nodiscard]] bool attackLaunched() const override { return script_.attackLaunched; }

    /// Which army this plays. Set once at match setup.
    void playFor(int army) noexcept { army_ = army; }
    [[nodiscard]] int army() const noexcept { return army_; }

private:
    const World* world_ = nullptr;
    int army_ = -1;
    rm::sim::Opponent script_;
    std::vector<Decision> decisions_;
};

} // namespace rm::ai
