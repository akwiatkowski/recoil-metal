#include "core/data/Opening.hpp"

#include "core/lua/LuaTable.hpp"

#include <fstream>
#include <iterator>

namespace rm::data {
namespace {

/// One `{ role = "extractor", tech = 1 }` entry.
[[nodiscard]] std::optional<OpeningStep> parseStep(const lua::Value& entry) {
    const lua::Value* role = entry.find("role");
    if (role == nullptr) {
        return std::nullopt;
    }
    const std::optional<unitdef::Role> parsed = unitdef::roleFromName(role->text);
    if (!parsed) {
        // A role this binary does not know. Refused rather than skipped: a newer data file on
        // an older engine must fail loudly, not open with three quarters of a plan.
        return std::nullopt;
    }

    OpeningStep step;
    step.role = *parsed;
    if (const lua::Value* tech = entry.find("tech")) {
        step.tech = static_cast<int>(tech->number);
    }
    const auto readTags = [](const lua::Value& list, std::vector<std::string>& into) {
        for (const lua::Value& tag : list.items) {
            if (!tag.text.empty()) {
                into.push_back(tag.text);
            }
        }
    };
    if (const lua::Value* requires_ = entry.find("requires")) {
        readTags(*requires_, step.requires_);
    }
    if (const lua::Value* fallback = entry.find("fallback")) {
        readTags(*fallback, step.fallback);
    }
    return step;
}

} // namespace

std::optional<Opening> parseOpening(const lua::Value& table) {
    Opening opening;

    if (const lua::Value* structures = table.find("structures")) {
        for (const lua::Value& entry : structures->items) {
            const std::optional<OpeningStep> step = parseStep(entry);
            if (!step) {
                return std::nullopt;
            }
            opening.structures.push_back(*step);
        }
    }

    if (const lua::Value* wave = table.find("wave")) {
        if (const lua::Value* unit = wave->find("unit")) {
            const std::optional<OpeningStep> step = parseStep(*unit);
            if (!step) {
                return std::nullopt;
            }
            opening.waveUnit = *step;
        }
        if (const lua::Value* size = wave->find("size")) {
            const double count = size->number;
            if (count <= 0.0) {
                return std::nullopt;  // a wave of nothing never launches
            }
            opening.waveSize = static_cast<std::size_t>(count);
        }
    }

    if (!opening.valid()) {
        return std::nullopt;
    }
    return opening;
}

std::optional<Opening> loadOpening(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    const std::string source{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
    const auto parsed = lua::parseTable(source);
    if (!parsed) {
        return std::nullopt;
    }
    return parseOpening(*parsed);
}

Opening defaultOpening() {
    // The five deleted constants, as values. Kept in C++ as well as in `data/opening.lua` so
    // that a build with no data directory still plays — and identical to the file, so the match
    // is the one the golden log records either way.
    Opening opening;
    opening.structures = {
        OpeningStep{unitdef::Role::Extractor, 1, {}, {}},   // ordered at spawn
        OpeningStep{unitdef::Role::Energy, 1, {}, {}},      // everything after is energy-bound
        OpeningStep{unitdef::Role::Extractor, 1, {}, {}},   // tanks are mass-bound
        // LAND, explicitly: the cheapest T1 factory is the AIR one, and an air factory builds
        // no tanks. A role alone is ambiguous here.
        OpeningStep{unitdef::Role::Factory, 1, {"LAND"}, {}},
    };
    // A TANK if the faction has one, otherwise any T1 land raider. Cybran fields no T1 tank,
    // and 40 bots do not close a match that 20 tanks do. See `data/opening.lua`.
    opening.waveUnit = OpeningStep{unitdef::Role::Raider, 1, {"LAND", "TANK"}, {"LAND"}};
    opening.waveSize = 20;  // derived for the tank; see data/opening.lua
    return opening;
}

} // namespace rm::data
