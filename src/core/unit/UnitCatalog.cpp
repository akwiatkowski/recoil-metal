#include "core/unit/UnitCatalog.hpp"

#include "core/lua/LuaTable.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace rm::unit {
namespace {

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// A field lookup that can also write. `Value::find` is read-only; the merge
/// needs the slot itself.
[[nodiscard]] lua::Field* fieldFor(lua::Value& table, std::string_view key) noexcept {
    for (lua::Field& field : table.fields) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}

void eraseField(lua::Value& table, std::string_view key) {
    std::erase_if(table.fields,
                  [key](const lua::Field& field) { return field.key == key; });
}

/// A decimal integer key, or nothing. Lua's `[1] = 'x'` and a positional `'x'`
/// are the same table entry, so a bracketed numeric key merges against the
/// array part — `C-220`'s by-index rule applies to it.
[[nodiscard]] std::optional<std::size_t> arrayKey(std::string_view key) noexcept {
    if (key.empty() || key.size() > 9) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (const char c : key) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    if (value == 0) {
        return std::nullopt;  // [0] is a hash key in Lua, not an array slot
    }
    return value;
}

/// One overlay item into the array part: index-wise, `nil` skipped, past the
/// end appended — the array half of `C-220`.
void mergeItem(lua::Value& base, std::size_t index, const lua::Value& overlay) {
    if (index < base.items.size()) {
        base.items[index] = merged(base.items[index], overlay);
    } else {
        // Retail's `t1[k] = v` writes past the end too — {'A'} merged with
        // {[2]='X'} yields {'A','X'}, holes and all. Our items vector cannot
        // hold a hole, so a gap lands as nil placeholders, which read back as
        // the same absent slots.
        while (base.items.size() < index) {
            base.items.emplace_back();
        }
        base.items.push_back(overlay);
    }
}

/// `UnitBlueprint`'s `SetShortId`: `gsub(lower(source), "^.*/([^/]+)_[a-z]+%.bp$", "%1")`
/// — the last path component minus its `_<letters>.bp` tail. A source that does
/// not match keeps the whole lowered path, which is what the gsub returns.
[[nodiscard]] std::string unitIdFrom(std::string_view source) {
    const std::string path = lowered(source);
    const std::size_t slash = path.rfind('/');
    const std::string_view file =
        slash == std::string::npos ? std::string_view{path} : std::string_view{path}.substr(slash + 1);

    constexpr std::string_view kSuffix = ".bp";
    if (file.size() > kSuffix.size() && file.ends_with(kSuffix)) {
        const std::string_view stem = file.substr(0, file.size() - kSuffix.size());
        const std::size_t underscore = stem.rfind('_');
        if (underscore != std::string_view::npos && underscore + 1 < stem.size()) {
            const std::string_view tail = stem.substr(underscore + 1);
            const bool letters = std::ranges::all_of(tail, [](char c) {
                return std::islower(static_cast<unsigned char>(c)) != 0;
            });
            if (letters) {
                return std::string{stem.substr(0, underscore)};
            }
        }
    }
    return path;
}

/// Whether the blueprint asks to merge rather than replace: `Merge` present
/// and truthy. `Merge = false` replaces like any absent flag — `false` is a
/// value, and only `true`-valued flags reach the merge branch in retail's
/// `if t[id] and bp.Merge`.
[[nodiscard]] bool wantsMerge(const lua::Value& bp) noexcept {
    const lua::Value* flag = bp.find("Merge");
    if (flag == nullptr) {
        return false;
    }
    if (flag->type == lua::Value::Type::None) {
        return false;
    }
    if (flag->type == lua::Value::Type::Bool) {
        return flag->boolean;
    }
    return true;  // any other value is truthy in Lua
}

/// The `<Name>Blueprint {` calls a `.bp` file makes, at top level only.
///
/// A scanner rather than a regex because the corpus writes `#` comments and
/// strings that can carry the words — `HelpText = 'Desert Blowing Sand'` is the
/// documented false positive — and because a file may hold MANY calls
/// (mods.scd's `all_units.bp` is a generated list of hundreds). Nested calls
/// inside a table are ignored: retail would run them as expressions, but no
/// shipped file does that, and top-level is where `doscript` semantics put the
/// constructors.
struct ConstructorCall {
    BlueprintGroup group;
    std::size_t brace;  ///< offset of the call's '{'
};

[[nodiscard]] std::vector<ConstructorCall> findConstructorCalls(std::string_view source) {
    std::vector<ConstructorCall> calls;
    std::size_t pos = 0;
    int depth = 0;

    const auto skipLineComment = [&]() {
        while (pos < source.size() && source[pos] != '\n') {
            ++pos;
        }
    };
    const auto skipLongBracket = [&]() {
        // At '['. [[...]] or [=[...]=]; returns false when it is not one.
        std::size_t level = 0;
        while (pos + 1 + level < source.size() && source[pos + 1 + level] == '=') {
            ++level;
        }
        if (pos + 1 + level >= source.size() || source[pos + 1 + level] != '[') {
            return false;
        }
        pos += 2 + level;
        while (pos < source.size()) {
            if (source[pos] == ']') {
                std::size_t closing = 0;
                while (pos + 1 + closing < source.size() && source[pos + 1 + closing] == '=') {
                    ++closing;
                }
                if (closing == level && pos + 1 + closing < source.size()
                    && source[pos + 1 + closing] == ']') {
                    pos += 2 + level;
                    return true;
                }
            }
            ++pos;
        }
        return true;
    };

    while (pos < source.size()) {
        const char c = source[pos];

        // Comments: `#` line comments are Supreme Commander's own dialect (the
        // table parser accepts them for the same reason), `--` line and
        // `--[[ ]]` long comments are Lua's.
        if (c == '#' || (c == '-' && pos + 1 < source.size() && source[pos + 1] == '-')) {
            if (c == '-') {
                pos += 2;
                if (pos < source.size() && source[pos] == '[' && skipLongBracket()) {
                    continue;
                }
            }
            skipLineComment();
            continue;
        }
        if (c == '\'' || c == '"') {
            const char quote = c;
            ++pos;
            while (pos < source.size() && source[pos] != quote) {
                pos += source[pos] == '\\' ? 2 : 1;
            }
            ++pos;  // the closing quote (or off the end, which parseTable reports)
            continue;
        }
        if (c == '[' && depth == 0) {
            // A long string at top level cannot hold a call; skip it if it is one.
            if (skipLongBracket()) {
                continue;
            }
        }
        if (c == '{') {
            ++depth;
            ++pos;
            continue;
        }
        if (c == '}') {
            depth = std::max(0, depth - 1);
            ++pos;
            continue;
        }
        if (depth == 0
            && (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_')) {
            const std::size_t start = pos;
            while (pos < source.size()
                   && (std::isalnum(static_cast<unsigned char>(source[pos])) != 0
                       || source[pos] == '_')) {
                ++pos;
            }
            const std::string_view name = source.substr(start, pos - start);
            std::size_t lookahead = pos;
            while (lookahead < source.size()
                   && std::isspace(static_cast<unsigned char>(source[lookahead])) != 0) {
                ++lookahead;
            }
            if (lookahead < source.size() && source[lookahead] == '{') {
                if (const std::optional<BlueprintGroup> group = blueprintGroupFor(name)) {
                    calls.push_back(ConstructorCall{*group, lookahead});
                }
            }
            continue;
        }
        ++pos;
    }
    return calls;
}

} // namespace

const char* blueprintConstructor(BlueprintGroup group) noexcept {
    switch (group) {
    case BlueprintGroup::Mesh:         return "MeshBlueprint";
    case BlueprintGroup::Unit:         return "UnitBlueprint";
    case BlueprintGroup::Prop:         return "PropBlueprint";
    case BlueprintGroup::Projectile:   return "ProjectileBlueprint";
    case BlueprintGroup::TrailEmitter: return "TrailEmitterBlueprint";
    case BlueprintGroup::Emitter:      return "EmitterBlueprint";
    case BlueprintGroup::Beam:         return "BeamBlueprint";
    }
    return nullptr;
}

std::optional<BlueprintGroup> blueprintGroupFor(std::string_view constructor) noexcept {
    for (const BlueprintGroup group :
         {BlueprintGroup::Mesh, BlueprintGroup::Unit, BlueprintGroup::Prop,
          BlueprintGroup::Projectile, BlueprintGroup::TrailEmitter, BlueprintGroup::Emitter,
          BlueprintGroup::Beam}) {
        if (constructor == blueprintConstructor(group)) {
            return group;
        }
    }
    return std::nullopt;
}

lua::Value merged(const lua::Value& base, const lua::Value& overlay) {
    // `for k,v in t2` never visits a nil: a merge cannot delete (`C-220`).
    if (overlay.type == lua::Value::Type::None) {
        return base;
    }
    // `type(t1)~='table' or type(t2)~='table' → t2`: a scalar overlay replaces
    // wholesale — including `false`, which is a value and takes effect.
    if (overlay.type != lua::Value::Type::Table
        || base.type != lua::Value::Type::Table) {
        return overlay;
    }

    lua::Value out = base;

    // The keyed part. A bracketed numeric key is an array slot in Lua, so it
    // merges by index like a positional item.
    for (const lua::Field& field : overlay.fields) {
        if (field.value.type == lua::Value::Type::None) {
            continue;  // nil does nothing — see above
        }
        if (const std::optional<std::size_t> index = arrayKey(field.key)) {
            mergeItem(out, *index - 1, field.value);
            continue;
        }
        if (lua::Field* slot = fieldFor(out, field.key)) {
            slot->value = merged(slot->value, field.value);
        } else {
            out.fields.push_back(field);
        }
    }

    // The array part, BY INDEX — `{'A','B','C'}` + `{'X'}` → `{'X','B','C'}`.
    for (std::size_t i = 0; i < overlay.items.size(); ++i) {
        if (overlay.items[i].type == lua::Value::Type::None) {
            continue;
        }
        mergeItem(out, i, overlay.items[i]);
    }
    return out;
}

std::string UnitCatalog::idFor(BlueprintGroup group, const lua::Value& bp,
                               std::string_view source) {
    // An authored BlueprintId wins for every group — `C-314`: "mod `.bp` files
    // with existing `BlueprintId` replace original". Retail's
    // `SetBackwardsCompatId` overwrites it unconditionally for non-unit groups,
    // contradicting the file's own header comment; the claim and the comment
    // agree, so the authored id is honoured here (see the header note).
    if (const std::optional<std::string_view> authored = bp.stringAt("BlueprintId")) {
        return lowered(*authored);
    }
    switch (group) {
    case BlueprintGroup::Unit:
        return unitIdFrom(source);
    case BlueprintGroup::Mesh: {
        std::string id = lowered(source);
        if (id.ends_with(".bp")) {
            id.erase(id.size() - 3);
        }
        return id;
    }
    default:
        // `SetBackwardsCompatId`: the whole lower-cased source, `.bp` included.
        return lowered(source);
    }
}

UnitCatalog::Outcome UnitCatalog::store(BlueprintGroup group, lua::Value bp,
                                        std::string_view source) {
    const std::string id = idFor(group, bp, source);
    std::map<std::string, lua::Value, std::less<>>& table = blueprints_[group];

    // `bp.Source = bp.Source or GetSource()` — the file it came from, kept for
    // the same reason retail keeps it: error messages name the source, and the
    // mesh-extraction defaults derive off it.
    if (bp.find("Source") == nullptr) {
        bp.fields.push_back(lua::Field{"Source", lua::Value{}});
        bp.fields.back().value.type = lua::Value::Type::Text;
        bp.fields.back().value.text = source;
    }

    const auto existing = table.find(id);
    if (existing != table.end() && wantsMerge(bp)) {
        // `bp.Merge = nil; bp.Source = nil` before the merge — the flag and the
        // overlay's provenance are not part of the result (`C-220`).
        eraseField(bp, "Merge");
        eraseField(bp, "Source");
        existing->second = merged(existing->second, bp);
        return Outcome::Merged;
    }

    // Last writer wins, silently — `C-220`: "duplicate `BlueprintId` is silent
    // last-writer-wins". A `Merge = true` blueprint with nothing to merge into
    // stores whole, flag and all, exactly as retail's else-branch leaves it.
    table.insert_or_assign(id, std::move(bp));
    return existing != table.end() ? Outcome::Replaced : Outcome::Stored;
}

std::size_t UnitCatalog::storeSource(std::string_view source, std::string_view path) {
    std::size_t stored = 0;
    for (const ConstructorCall& call : findConstructorCalls(source)) {
        // The table literal is the call's argument; parseTable skips to it.
        auto parsed = lua::parseTable(source.substr(call.brace));
        if (!parsed) {
            continue;  // a broken .bp is pcall'd and skipped (`C-220`)
        }
        store(call.group, std::move(*parsed), path);
        ++stored;
    }
    return stored;
}

std::size_t UnitCatalog::loadBlueprints(const vfs::Vfs& content,
                                        const std::vector<vfs::ActiveMod>& mods) {
    std::size_t stored = 0;
    const auto scan = [&](std::string_view directory) {
        for (const std::string& path : content.list(directory, ".bp")) {
            if (const auto bytes = content.read(path)) {
                stored += storeSource(
                    {reinterpret_cast<const char*>(bytes->data()), bytes->size()}, path);
            }
        }
    };

    // `C-270`: the fixed directory scan, in retail's order — effects, env.meshes,
    // projectiles, props, units. `list` answers alphabetically within each, which
    // is the deterministic order the category bits rely on.
    for (const char* directory :
         {"/effects", "/env/meshes", "/projectiles", "/props", "/units"}) {
        scan(directory);
    }

    // `C-314`: then every active mod's `.bp` files under its `/mods/<name>`
    // mount, in `__active_mods` order — before `ModBlueprints` would run, which
    // is the hook this store's `store` already models.
    for (const vfs::ActiveMod& mod : mods) {
        scan(vfs::modMountPoint(mod));
    }
    return stored;
}

const lua::Value* UnitCatalog::find(BlueprintGroup group, std::string_view id) const {
    const auto table = blueprints_.find(group);
    if (table == blueprints_.end()) {
        return nullptr;
    }
    const auto it = table->second.find(lowered(id));
    return it != table->second.end() ? &it->second : nullptr;
}

std::vector<UnitCatalog::Entry> UnitCatalog::registrationOrder() const {
    std::vector<Entry> out;
    for (const BlueprintGroup group :
         {BlueprintGroup::Mesh, BlueprintGroup::Unit, BlueprintGroup::Prop,
          BlueprintGroup::Projectile, BlueprintGroup::TrailEmitter, BlueprintGroup::Emitter,
          BlueprintGroup::Beam}) {
        const auto table = blueprints_.find(group);
        if (table == blueprints_.end()) {
            continue;
        }
        for (const auto& [id, bp] : table->second) {
            out.push_back(Entry{group, id, &bp});
        }
    }
    return out;
}

std::size_t UnitCatalog::size(BlueprintGroup group) const {
    const auto table = blueprints_.find(group);
    return table != blueprints_.end() ? table->second.size() : 0;
}

} // namespace rm::unit
