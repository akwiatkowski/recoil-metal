#pragma once

// A Lua 5.4 implementation of `rm::sim::ScriptTaskHost` (`C-228`, `C-318`).
//
// WHAT THIS IS FOR. The sim's `ScriptTaskHost` port is the seam at which a real Lua runtime
// (rather than the `RecordingTasks` test stub or `EnhancementTasks` built-ins) drives a
// task's lifecycle: `OnPreCreate` / `OnCreate` once on creation, `TaskTick(result)` once per
// scheduler beat, and `OnDestroy` once when the queue retires the order. Every shop task the
// engine ships is one Lua file with those three (now four) globals; this host lets a test
// execute one such file through the port and observe its lifecycle.
//
// WHY `registerTask` TAKES THE SCRIPT'S SOURCE. `C-268` says mods cannot shadow, but the
// sim's script tasks are loaded by module path — not by class name — so a test fixture
// mounts a scratch directory into a `vfs::Vfs` and points the host at it the same way
// `loadWeaponVisuals` does. `register` short-circuits that for tiny fixtures and lets a
// test inline the source it would otherwise write to disk.
//
// BOUND TO ONE VM AND ONE CONTENT TREE. The bootstrap, the loaded modules, and the
// instantiated class tables all live inside one `lua_State`. Two hosts = two VMs; the
//registry in one VM does not bleed into the other.
//
// NOT FOR THE LIVE APP YET. The FAF-AI path has its own `lua_State` inside `FafAi`, and
// hosting a real `<TaskName>.lua` from the retail corpus against this port is a future
// pass — the deliverable C-228 names is the seam, not the shipping integration.

#include "core/sim/ScriptTask.hpp"
#include "core/vfs/Vfs.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace rm::app {

/// One task's Lua source registered with the host.
///
/// A registered source takes the fast path; the `modulePath` form lets the same task name
/// resolve a VFS module when the task is actually shipped through a blueprint path.
struct ScriptTaskRegistration {
    /// The module path the Lua `import` would use (`/lua/...`) when the source is registered
    /// out of a VFS, empty when `source` is inlined.
    std::string modulePath;
    /// The Lua source. Loaded inside an environment named `modulePath` (or `taskName` when
    /// `modulePath` is empty); `TypeClass = Class(...) { ... }` is the binding the host reads
    /// on first dispatch.
    std::string source;
};

/// A `ScriptTaskHost` driven by a Lua 5.4 VM (`C-228`) and a `vfs::Vfs` content tree.
///
/// Tasks are dispatched by NAME; the host keeps a small registry that maps a task name
/// either to an inlined Lua source or to a module path it `import`s from `content` on first
/// use. The chosen source declares a class assigned to `TypeClass`; the host instantiates
/// one table per `(unit, task)` invocation and threads the four hooks — `OnPreCreate`,
/// `OnCreate`, `TaskTick(result)`, `OnDestroy` — through its methods.
///
/// The host's lifetime is bound to the lifetime of `content`: the bootstrap holds onto a
/// pointer rather than copying the VFS, so the VFS must outlive every script task the host
/// dispatches.
class ScriptHost final : public rm::sim::ScriptTaskHost {
public:
    explicit ScriptHost(const rm::vfs::Vfs& content);
    ~ScriptHost() override;

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    /// Registers a task by `taskName` → `reg`. The task is dispatched the same way it would
    /// be in retail: a class table `TypeClass` declared by the source, instantiated once per
    /// dispatch, fed the four hook calls.
    void registerTask(std::string_view taskName, ScriptTaskRegistration reg);

    /// Publishes the match's mod list into the host VM (`C-313`): orders `mods`
    /// by `before`/`after` uid constraints (uid-alphabetical otherwise) and
    /// installs the resulting table as both `_G.__active_mods` and
    /// `_G.gameInfo.GameMods`, mirroring retail's native build from
    /// `gameInfo.GameMods` (`0x0052fece`). Mounting stays with the caller: the
    /// host borrows `content_` read-only, so each mod's `mountMod` (which
    /// registers its hookdir for the `C-311` concat) must happen on the VFS
    /// before dispatch. Each entry carries uid, name, location (the VFS mount
    /// point), ui_only, and hookdir, matching `mods.lua`'s `LoadModInfo`.
    void setActiveMods(std::vector<rm::vfs::ActiveMod> mods);

    /// Modules loaded but whose instantiation errored. Empty until `registerTask` or a
    /// first-time dispatch fails; exposed for the test fixture to assert exactly-once
    /// behaviour and the absence of errors.
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept;

    // --- The ScriptTaskHost lifecycle (`C-228`, `C-318`) -----------------
    void onPreCreate(rm::sim::UnitId unit, std::string_view task,
                     std::span<const std::uint8_t> commandData,
                     rm::sim::ScriptTaskState& state) noexcept override;
    void onCreate(rm::sim::UnitId unit, std::string_view task,
                  std::span<const std::uint8_t> commandData,
                  rm::sim::ScriptTaskState& state) override;
    [[nodiscard]] std::int32_t taskTick(rm::sim::UnitId unit, std::string_view task,
                                        std::span<const std::uint8_t> commandData,
                                        rm::sim::ScriptTaskState& state) override;
    void onDestroy(rm::sim::UnitId unit, std::string_view task,
                   std::span<const std::uint8_t> commandData,
                   rm::sim::ScriptTaskState& state) override;

private:
    struct Instance {
        std::uint32_t self = 0;
    };

    [[nodiscard]] Instance ensureInstance(rm::sim::UnitId unit, std::string_view task,
                                          rm::sim::ScriptTaskState& state,
                                          std::span<const std::uint8_t> commandData);

    const rm::vfs::Vfs& content_;
    lua_State* lua_;  // owned through `impl_->state`; bare pointer for hot-path lookup
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rm::app
