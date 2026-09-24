#pragma once

#include "core/sim/IdPool.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rm::sim {

/// Retail task-scheduler results (`C-188`). Positive values are beat delays, while the named
/// negative and zero values control the current scheduler microstep.
enum class ScriptTaskStatus : std::int32_t {
    Delay = -4,
    Abort = -3,
    Suspend = -2,
    Done = -1,
    Repeat = 0,
    NextBeat = 1,
};

/// Serializable native ownership for one Lua-backed task.
///
/// The byte vector is deliberately opaque to the sim. A Lua adapter decides how to encode its
/// object; command dispatch only owns when that state is created, ticked, slept, and destroyed.
struct ScriptTaskState {
    bool created = false;
    bool suspended = false;
    std::uint32_t sleepBeats = 0;
    std::int32_t aiResult = 0;
    std::vector<std::uint8_t> opaque;
};

/// Port implemented by a script runtime, kept out of the deterministic simulation core.
///
/// The host must outlive every CommandQueue it is bound to. `state` is the complete mutable
/// task object for save/load purposes; the host must not retain a pointer to it because deque
/// edits may move the owning command.
///
/// `C-318`: retail calls `OnPreCreate` before any native setup (the script may prepare
/// fields the constructor expects to find) and `OnCreate` after; `OnDestroy` arrives at the
/// tail of `Entity::OnDestroy`. The default `onPreCreate` is a no-op so existing hosts keep
/// working — only the dispatcher that wants the new ordering needs to override it.
class ScriptTaskHost {
public:
    virtual ~ScriptTaskHost() = default;

    /// Optional pre-creation hook (`C-318`). Fires once, just before `onCreate`, and is the
    /// last chance the script has to shape state before native setup. Default no-op.
    virtual void onPreCreate(UnitId, std::string_view,
                             std::span<const std::uint8_t>,
                             ScriptTaskState&) noexcept {}

    virtual void onCreate(UnitId unit, std::string_view task,
                          std::span<const std::uint8_t> commandData,
                          ScriptTaskState& state) = 0;
    [[nodiscard]] virtual std::int32_t taskTick(
        UnitId unit, std::string_view task, std::span<const std::uint8_t> commandData,
        ScriptTaskState& state) = 0;
    virtual void onDestroy(UnitId unit, std::string_view task,
                           std::span<const std::uint8_t> commandData,
                           ScriptTaskState& state) = 0;
};

} // namespace rm::sim
