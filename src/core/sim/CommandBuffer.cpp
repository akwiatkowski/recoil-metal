// COMMAND INTAKE — what the world says, in the order it said it.
//
// `CommandBuffer::submit` is the front door every producer shares: a human's click, a script's
// decision, a replayed log line. It canonicalizes the recipient set and stamps a serial, so
// "they replay through the same path" is mechanical rather than aspirational. `take` drains
// the issues due on one tick/phase boundary.
//
// What this file does NOT do is judge content — whether a command can be carried out is
// `CommandApply.cpp`'s question, asked later, at the mutation boundary.
#include "core/sim/Command.hpp"
#include "core/sim/CommandInternal.hpp"

#include "core/sim/UnitStore.hpp"

#include <algorithm>

namespace rm::sim {

std::optional<CommandId> CommandBuffer::submit(CommandIssue issue, UnitStore& store) {
    if (!validCancellation(issue) || issue.source == kInvalidCommandSource
        || issue.player != static_cast<PlayerIndex>(issue.source) || issue.count == 0
        || (issue.kind == CommandKind::Script
            && (issue.scriptTask.empty()
                || issue.scriptTask.size() > kMaxScriptTaskNameBytes
                || issue.scriptData.size() > kMaxScriptTaskDataBytes))
        || (issue.kind != CommandKind::Script
            && (!issue.scriptTask.empty() || !issue.scriptData.empty()))) {
        return std::nullopt;
    }
    const bool explicitId = issue.id != kInvalidCommandId;
    if (!explicitId) {
        const std::optional<CommandId> allocated = store.allocateCommandId(issue.source);
        if (!allocated) {
            return std::nullopt;
        }
        issue.id = *allocated;
    } else if (!store.consumeCommandId(issue.source, issue.id)) {
        return std::nullopt;
    }
    canonicalizeUnits(issue.units);
    // Live input cannot address nobody. Replay may carry an explicit empty accepted set because
    // the original submission still consumed an ID, which is authoritative allocator state.
    if (issue.units.empty() && !explicitId) {
        return std::nullopt;
    }
    const CommandId id = issue.id;
    pending_.push_back(std::move(issue));
    return id;
}

std::vector<CommandIssue> CommandBuffer::take(TickIndex tick, CommandPhase phase) {
    std::vector<CommandIssue> due;
    std::vector<CommandIssue> waiting;
    due.reserve(pending_.size());
    waiting.reserve(pending_.size());
    for (CommandIssue& issue : pending_) {
        if (issue.tick == tick && issue.phase == phase) {
            due.push_back(std::move(issue));
        } else {
            waiting.push_back(std::move(issue));
        }
    }
    pending_ = std::move(waiting);
    std::stable_sort(due.begin(), due.end(), [](const CommandIssue& a, const CommandIssue& b) {
        return a.source < b.source;
    });
    return due;
}

} // namespace rm::sim
