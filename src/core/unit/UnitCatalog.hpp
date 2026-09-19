#pragma once

// The blueprint table store: retail's `original_blueprints` from
// `mohodata.scd:lua/system/Blueprints.lua`, as C++.
//
// WHY THIS EXISTS (`C-220`, `C-314`). A Supreme Commander mod does not shadow
// blueprint files — `C-268` refutes VFS shadowing for mods — it ships `.bp`
// files that the loader runs AFTER the base scan, and each one either REPLACES
// the blueprint with the same id or, when it carries `Merge = true`, DEEP
// MERGES into it. That merge is the whole of what `Merge = true` means, and it
// is defined on the blueprint TABLE, not on the `UnitDef` a unit blueprint
// later becomes — so it lives here, between `lua::parseTable` and
// `unitbp::load`, where the table still exists. `rm::sim::UnitCatalog` is the
// def registry downstream of this; the two share a name because they are the
// same idea at two stages of the pipeline.
//
// THE RETAIL RULES, from `table.merged` (system/utils.lua:132) and
// `StoreBlueprint` (Blueprints.lua:86):
//
//   * recursive DEEP merge — a table field merges into the same field, it does
//     not replace it;
//   * arrays merge BY INDEX — `{'A','B','C'}` overlaid with `{'X'}` gives
//     `{'X','B','C'}`, not a replace and not an append (extra overlay items DO
//     append past the base's length);
//   * a `nil` value does NOTHING — `for k,v in t2` never visits one, so a field
//     cannot be deleted by a merge (FAF later added a `'__nil'` sentinel for
//     exactly this; that is a FAF extension, not retail);
//   * `false` takes effect — it is a value, not an absence;
//   * `Merge = true` only merges when a same-id blueprint already exists —
//     otherwise the file stores whole, flag and all;
//   * there is NO base/parent inheritance: all 568 shipped `*_unit.bp` are flat
//     literals and `Merge` appears zero times in the shipped corpus. This is
//     machinery for mods, not for the stock game.
//
// GROUPS AND IDS follow the retail constructors. A `.bp` file is a call —
// `UnitBlueprint { ... }`, `PropBlueprint { ... }` — and the constructor name
// IS the group. The id defaults off the source path per group (units take the
// basename, everything else the lower-cased path), with an authored
// `BlueprintId` honoured first — `C-314`: "mod `.bp` files with existing
// `BlueprintId` replace original". Retail's `SetBackwardsCompatId` overwrites
// the field unconditionally for non-unit groups, which contradicts its own
// header comment ("the mod must set the BlueprintId field"); the comment and
// the claim agree, so the authored id wins here.

#include "core/lua/LuaValue.hpp"
#include "core/vfs/Vfs.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rm::unit {

/// The seven blueprint groups, in retail's registration order (`C-270`:
/// `RegisterAllBlueprints` walks them in exactly this sequence, and the order
/// is load-bearing because category ordinals are assigned as it runs).
enum class BlueprintGroup : std::uint8_t {
    Mesh,
    Unit,
    Prop,
    Projectile,
    TrailEmitter,
    Emitter,
    Beam,
};

/// The constructor name a `.bp` file calls for a group — `UnitBlueprint`,
/// `PropBlueprint` — or nullptr for a name that is none of them.
[[nodiscard]] const char* blueprintConstructor(BlueprintGroup group) noexcept;
[[nodiscard]] std::optional<BlueprintGroup> blueprintGroupFor(
    std::string_view constructor) noexcept;

/// `table.merged(t1, t2)` — the deep merge `C-220` pins. `overlay` wins;
/// tables recurse; arrays merge by index; `nil` overlay values are skipped so
/// nothing can be deleted; `false` lands like any other value. Neither
/// argument is modified — retail's copy-on-write collapsed to a plain copy,
/// which is the same result without the aliasing its comment warns about.
[[nodiscard]] lua::Value merged(const lua::Value& base, const lua::Value& overlay);

/// The store `LoadBlueprints` fills: every blueprint the scan produced, keyed
/// by group and id, in the order registration will walk them.
///
/// Ids are case-folded on the way in — retail's `SetShortId`/`SetLongId`
/// lower-case every derived id, and an authored `BlueprintId` is compared
/// against them, so the store's key space is lower-case throughout.
class UnitCatalog {
public:
    /// What `store` did with one blueprint.
    enum class Outcome : std::uint8_t {
        Stored,   ///< no same-id blueprint existed — stored whole
        Replaced, ///< same id, no `Merge` flag — last writer wins (`C-220`)
        Merged,   ///< same id and `Merge = true` — deep-merged into the base
    };

    /// `StoreBlueprint(group, bp)`: files `bp` under its id in `group`.
    ///
    /// `source` is the VFS path the table came from — the id default and the
    /// `Source` field both derive from it, exactly as retail's `GetSource()`
    /// supplies them. An authored `Source` is kept.
    Outcome store(BlueprintGroup group, lua::Value bp, std::string_view source);

    /// Runs one `.bp` file: finds each `<Group>Blueprint { ... }` constructor
    /// at top level and stores its table. A file may hold several calls —
    /// mods.scd's `all_units.bp` carries hundreds — and a table that fails to
    /// parse skips that one call rather than the file, which is retail's
    /// `safecall`/`pcall` boundary (`C-220`: "a broken `.bp` is pcall'd and
    /// skipped"). Returns how many blueprints were stored.
    std::size_t storeSource(std::string_view source, std::string_view path);

    /// `LoadBlueprints` (`C-270`/`C-314`): scans the fixed directories in
    /// retail's order — `{effects, env.meshes, projectiles, props, units}` —
    /// then every active mod's `.bp` files under its `/mods/<name>` mount in
    /// `__active_mods` order, feeding each file to `storeSource`. This is the
    /// pass that makes a mod's blueprints real: a mod `.bp` with an existing
    /// `BlueprintId` replaces the original, `Merge = true` deep-merges onto it
    /// (`C-220`). Returns how many blueprints were stored.
    std::size_t loadBlueprints(const vfs::Vfs& content,
                               const std::vector<vfs::ActiveMod>& mods = {});

    /// One stored blueprint, or nullptr. `id` is case-folded before lookup.
    [[nodiscard]] const lua::Value* find(BlueprintGroup group,
                                         std::string_view id) const;

    /// Every blueprint in registration order (`C-270`): the fixed group order
    /// Mesh→Unit→Prop→Projectile→TrailEmitter→Emitter→Beam, alphabetical by id
    /// within each group — retail's `sortedpairs`. Deterministic, which is the
    /// property category-bit assignment relies on.
    struct Entry {
        BlueprintGroup group;
        std::string_view id;
        const lua::Value* blueprint;
    };
    [[nodiscard]] std::vector<Entry> registrationOrder() const;

    [[nodiscard]] std::size_t size(BlueprintGroup group) const;

private:
    /// The id a blueprint files under: its authored `BlueprintId` when present,
    /// else the group's default off the source path — basename for units
    /// (`UEL0201_unit.bp` → `uel0201`), the lower-cased path minus `.bp` for
    /// meshes, the lower-cased path for the rest.
    [[nodiscard]] static std::string idFor(BlueprintGroup group, const lua::Value& bp,
                                           std::string_view source);

    // `std::map` rather than unordered: registration order is alphabetical
    // within a group, and keeping the store sorted makes that order the
    // iteration order rather than a sort at read time.
    std::map<BlueprintGroup, std::map<std::string, lua::Value, std::less<>>> blueprints_;
};

} // namespace rm::unit
