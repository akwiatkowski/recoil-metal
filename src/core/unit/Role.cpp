#include "core/unit/Role.hpp"

#include <array>
#include <utility>

namespace rm::unitdef {
namespace {

/// The name each role goes by in a data file. Index-locked to the enum, asserted below.
inline constexpr std::array<std::pair<Role, std::string_view>, 23> kNames{{
    {Role::Unknown, "unknown"},
    {Role::Civilian, "civilian"},
    {Role::Commander, "commander"},
    {Role::Builder, "builder"},
    {Role::Factory, "factory"},
    {Role::Extractor, "extractor"},
    {Role::Energy, "energy"},
    {Role::Storage, "storage"},
    {Role::Defence, "defence"},
    {Role::Raider, "raider"},
    {Role::Assault, "assault"},
    {Role::Artillery, "artillery"},
    {Role::AntiAir, "antiair"},
    {Role::AntiNaval, "antinaval"},
    {Role::Air, "air"},
    {Role::Bomber, "bomber"},
    {Role::Naval, "naval"},
    {Role::Sub, "sub"},
    {Role::Scout, "scout"},
    {Role::Transport, "transport"},
    {Role::Shield, "shield"},
    {Role::Radar, "radar"},
    {Role::Experimental, "experimental"},
}};

static_assert(kNames.size() == static_cast<std::size_t>(Role::Experimental) + 1,
              "every Role needs a name, and no name may outlive its Role — a data file that "
              "named a role the enum had lost would silently classify as unknown");

} // namespace

std::string_view roleName(Role role) noexcept {
    const auto index = static_cast<std::size_t>(role);
    return index < kNames.size() ? kNames[index].second : kNames.front().second;
}

std::optional<Role> roleFromName(std::string_view name) noexcept {
    for (const auto& [role, spelling] : kNames) {
        if (spelling == name) {
            return role;
        }
    }
    return std::nullopt;
}

int techOf(const UnitDef& def) noexcept {
    // Experimental first: an experimental declares `EXPERIMENTAL` and no `TECHn`, so testing
    // the tiers first would report zero for the biggest things in the game.
    if (def.hasCategory("EXPERIMENTAL")) {
        return 4;
    }
    if (def.hasCategory("TECH3")) {
        return 3;
    }
    if (def.hasCategory("TECH2")) {
        return 2;
    }
    if (def.hasCategory("TECH1")) {
        return 1;
    }
    return 0;
}

Role roleOf(const UnitDef& def) noexcept {
    // See the header for why the ORDER is the design. Reading this top to bottom is reading
    // the classifier.

    // 1. A commander is a commander before it is anything else. It builds, it makes mass and
    //    energy, it carries a gun — and losing it ends the match, so nothing else it does
    //    competes.
    if (def.hasCategory("COMMAND")) {
        return Role::Commander;
    }

    // 2. An experimental's role in a build order is "the big one", whatever it does.
    if (def.hasCategory("EXPERIMENTAL")) {
        return Role::Experimental;
    }

    // 3. Scenery is scenery even when it has a gun. A campaign map's defended village carries
    //    DIRECTFIRE and STRUCTURE, and without this it would classify as a defence and be
    //    eligible for a build order.
    if (def.hasCategory("CIVILIAN")) {
        return Role::Civilian;
    }

    // 4. The economic kinds, before the military ones: a mass extractor with a token gun is
    //    still an extractor.
    //
    //    BY CATEGORY ONLY, and this cost a bug to learn. The first version also accepted a
    //    non-zero production or storage FIGURE here, on the reasoning that 07 §4.4 derives
    //    `extractsmetal` from both and a mod might state only one. That is true of the
    //    CAPABILITY and wrong for the ROLE: in the shipped corpus a land factory carries
    //    `StorageEnergy` and a T1 engineer carries both, so "has any storage" classified
    //    UEB0101 and UEL0105 as `storage`. A factory with a hundred energy of buffer is a
    //    factory.
    //
    //    The figure fallback still exists — it moved to the END, where it catches content that
    //    declares a figure and no role category at all, without letting an incidental buffer
    //    outvote an explicit tag.
    if (def.hasCategory("MASSEXTRACTION")) {
        return Role::Extractor;
    }
    if (def.hasCategory("ENERGYPRODUCTION")) {
        return Role::Energy;
    }
    if (def.hasCategory("ENERGYSTORAGE") || def.hasCategory("MASSSTORAGE")) {
        return Role::Storage;
    }

    // 5. A factory is a builder that cannot move, and a build order needs the distinction: it
    //    queues units at a factory and structures with an engineer.
    if (def.hasCategory("FACTORY")) {
        return Role::Factory;
    }
    if (def.hasCategory("ENGINEER") || def.hasCategory("CONSTRUCTION")) {
        return Role::Builder;
    }

    // 6. An explicit scout is a scout even when it carries radar. Mobile scouts are the
    //    common counterexample to classifying every RADAR carrier as a radar installation.
    if (def.hasCategory("SCOUT")) {
        return Role::Scout;
    }

    // The remaining support kinds precede the combat ones because a radar with a gun is still
    // what you build for the radar.
    if (def.hasCategory("SHIELD") || def.hasCategory("SHIELDDOME")) {
        return Role::Shield;
    }
    if (def.hasCategory("RADAR") || def.hasCategory("SONAR")
        || def.hasCategory("OMNI")) {
        return Role::Radar;
    }
    if (def.hasCategory("TRANSPORTATION")) {
        return Role::Transport;
    }
    // 7. A STRUCTURE that shoots is a defence, whatever it shoots at — the distinction a
    //    build order cares about is "does this hold ground", and that is what being immobile
    //    and armed means. Tested before the weapon-class kinds so a point-defence turret does
    //    not come out as `assault`.
    const bool immobile = def.hasCategory("STRUCTURE") && !def.hasCategory("MOBILE");
    if (immobile && (def.hasCategory("DEFENSE") || def.hasCategory("DIRECTFIRE")
                     || def.hasCategory("ANTIAIR") || def.hasCategory("ARTILLERY")
                     || def.hasCategory("INDIRECTFIRE") || def.hasCategory("SILO"))) {
        return Role::Defence;
    }

    // 8. The domain kinds. Air before the weapon classes, because what a build order needs
    //    from an aircraft is that it flies; a bomber is the one air unit specific enough to
    //    name separately.
    if (def.hasCategory("AIR")) {
        if (def.hasCategory("BOMBER")) {
            return Role::Bomber;
        }
        if (def.hasCategory("ANTIAIR")) {
            return Role::AntiAir;
        }
        return Role::Air;
    }
    if (def.hasCategory("SUBMERSIBLE")) {
        return Role::Sub;
    }
    if (def.hasCategory("NAVAL")) {
        return Role::Naval;
    }

    // 9. Finally the ground weapon classes, most specific outward.
    if (def.hasCategory("ANTIAIR")) {
        return Role::AntiAir;
    }
    if (def.hasCategory("ANTINAVY")) {
        return Role::AntiNaval;
    }
    // `INDIRECTFIRE` is the corpus's own tag for lobbing things, and `SILO` for the strategic
    // launchers — both are artillery as far as a build order is concerned: the long-range one.
    if (def.hasCategory("ARTILLERY") || def.hasCategory("INDIRECTFIRE")
        || def.hasCategory("SILO")) {
        return Role::Artillery;
    }
    if (def.hasCategory("DIRECTFIRE")) {
        // A raider is fast and light, an assault unit is neither. Tech is the cheap proxy the
        // corpus supports: T1 direct fire is what raids, T2 and up is what assaults. Better
        // would be speed against the family median, which needs the whole corpus loaded —
        // this is honest about being a proxy rather than pretending to measure.
        return techOf(def) <= 1 ? Role::Raider : Role::Assault;
    }

    // 10. Last, capability FIGURES — for content that states one and declares no category this
    //     classifier recognises. Deliberately after everything: a figure is weaker evidence
    //     than a tag. In particular the Cybran Mantis has BuildRate=1 for its auxiliary repair
    //     arm, but DIRECTFIRE is still its primary role.
    if (def.isBuilder()) {
        return Role::Builder;
    }
    if (def.producesMassPerSecond > 0.0f) {
        return Role::Extractor;
    }
    if (def.producesEnergyPerSecond > 0.0f) {
        return Role::Energy;
    }
    if (def.storageMass > sim::Mag{} || def.storageEnergy > sim::Mag{}) {
        return Role::Storage;
    }

    return Role::Unknown;
}

} // namespace rm::unitdef
