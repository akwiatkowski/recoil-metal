#include "core/unit/Armor.hpp"

#include <algorithm>
#include <cmath>

namespace rm::unitdef {
namespace {

/// The name `default` occupies, spelled once.
constexpr std::string_view kDefaultName = "default";

/// ASCII lower-casing, done by hand rather than through `std::tolower`.
///
/// TWO REASONS, and the second is the one that matters. `std::tolower` takes an `int` and is
/// undefined for negative values, so a `char` above 127 — which a mod's file may well contain —
/// is a bug waiting for the right content. And it is LOCALE-DEPENDENT: in a Turkish locale
/// `tolower('I')` is a dotless i, so the same blueprint would resolve to different armour
/// classes on two machines with different environments. That is a determinism bug arriving
/// through `setlocale`, which is exactly the kind this project exists to not have.
[[nodiscard]] char foldChar(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string fold(std::string_view name) {
    std::string folded;
    folded.reserve(name.size());
    for (const char c : name) {
        folded.push_back(foldChar(c));
    }
    return folded;
}

[[nodiscard]] bool foldedEqual(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (foldChar(a[i]) != foldChar(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

ArmorRegistry::ArmorRegistry() : names_{std::string{kDefaultName}} {}

ArmorRegistry ArmorRegistry::fromNames(std::span<const std::string_view> names) {
    ArmorRegistry registry;

    std::vector<std::string> folded;
    folded.reserve(names.size());
    for (const std::string_view name : names) {
        if (name.empty()) {
            continue;
        }
        std::string lowered = fold(name);
        if (lowered == kDefaultName) {
            // Already class 0. Listing it is legal and common — Supreme Commander's own table
            // opens with `Default` — and it must not become a second class.
            continue;
        }
        folded.push_back(std::move(lowered));
    }

    std::sort(folded.begin(), folded.end());
    folded.erase(std::unique(folded.begin(), folded.end()), folded.end());

    // 255 classes plus `default` is the ceiling `ArmorClass` can name (`core/Types.hpp`).
    // Truncating is the honest failure: a class that cannot be addressed is better than two
    // classes sharing an index, which would make one of them silently take the other's damage.
    constexpr std::size_t kMaxClasses = 256;
    if (folded.size() > kMaxClasses - 1) {
        folded.resize(kMaxClasses - 1);
    }

    registry.names_.reserve(folded.size() + 1);
    for (std::string& name : folded) {
        registry.names_.push_back(std::move(name));
    }
    return registry;
}

ArmorClass ArmorRegistry::classFor(std::string_view name) const noexcept {
    if (name.empty()) {
        return kDefaultArmor;
    }
    const std::string lowered = fold(name);
    if (lowered == kDefaultName) {
        return kDefaultArmor;
    }
    // `names_[1..]` is sorted, so this is a binary search. `names_[0]` is deliberately outside
    // it: `default` is pinned to index 0 and would not sort there.
    const auto begin = names_.begin() + 1;
    const auto found = std::lower_bound(begin, names_.end(), lowered);
    if (found == names_.end() || *found != lowered) {
        return kDefaultArmor;
    }
    return static_cast<ArmorClass>(found - names_.begin());
}

bool ArmorRegistry::knows(std::string_view name) const noexcept {
    if (name.empty()) {
        return false;
    }
    const std::string lowered = fold(name);
    if (lowered == kDefaultName) {
        return true;
    }
    const auto begin = names_.begin() + 1;
    const auto found = std::lower_bound(begin, names_.end(), lowered);
    return found != names_.end() && *found == lowered;
}

std::string_view ArmorRegistry::name(ArmorClass armor) const noexcept {
    return armor < names_.size() ? std::string_view{names_[armor]} : std::string_view{};
}

sim::Mag DamageProfile::against(ArmorClass armor) const noexcept {
    for (std::uint8_t i = 0; i < overrideCount; ++i) {
        if (overrideArmor[i] == armor) {
            return overrideDamage[i];
        }
    }
    return base;
}

bool DamageProfile::addOverride(ArmorClass armor, sim::Mag damage) noexcept {
    if (armor == kDefaultArmor) {
        // `base` is what an override against class 0 would mean, and having two ways to say it
        // is how they come to disagree.
        return false;
    }
    for (std::uint8_t i = 0; i < overrideCount; ++i) {
        if (overrideArmor[i] == armor) {
            overrideDamage[i] = damage;
            return true;
        }
    }
    if (overrideCount >= kMaxOverrides) {
        return false;
    }
    overrideArmor[overrideCount] = armor;
    overrideDamage[overrideCount] = damage;
    ++overrideCount;
    return true;
}

bool DamageProfile::harmful() const noexcept {
    if (base > sim::Mag{}) {
        return true;
    }
    for (std::uint8_t i = 0; i < overrideCount; ++i) {
        if (overrideDamage[i] > sim::Mag{}) {
            return true;
        }
    }
    return false;
}

bool operator==(const DamageProfile& a, const DamageProfile& b) noexcept {
    if (a.base != b.base || a.overrideCount != b.overrideCount
        || a.paralyze.value != b.paralyze.value) {
        return false;
    }
    // Order-sensitive on purpose. Two profiles that list the same overrides in a different
    // order came from different content, and a comparison that called them equal would make a
    // round-trip test pass while the importer reordered things.
    for (std::uint8_t i = 0; i < a.overrideCount; ++i) {
        if (a.overrideArmor[i] != b.overrideArmor[i]
            || a.overrideDamage[i] != b.overrideDamage[i]) {
            return false;
        }
    }
    return true;
}

DamageProfile flatDamage(sim::Mag damage) noexcept {
    DamageProfile profile;
    profile.base = damage;
    return profile;
}

DamageProfile damageFromMatrix(sim::Mag damage, std::string_view damageType,
                               std::span<const ArmorMultiplier> matrix) {
    DamageProfile profile = flatDamage(damage);

    for (const ArmorMultiplier& row : matrix) {
        if (!foldedEqual(row.damageType, damageType)) {
            continue;
        }
        if (row.armor == kDefaultArmor) {
            // A multiplier against `default` would have to change `base`, and nothing in the
            // shipped table does it — `Default` lists only `Normal 1.0`. Skipped rather than
            // applied, so that an unexpected one is a no-op instead of a silent global rescale.
            continue;
        }
        // 1.0 is the value an ABSENT row already means, so writing it down would spend an
        // override slot to say nothing. This is what keeps 478 of 494 weapons at zero
        // overrides even though `Normal 1.0` is stated explicitly for most classes.
        if (row.multiplier == 1.0f) {
            continue;
        }
        // The multiply happens HERE, at load, and in `double` — which is the legitimate float
        // boundary (§5.2): content arrives as floats and is converted exactly once, and nothing
        // downstream ever sees one.
        //
        // ON THE RAW INTEGER rather than through `Mag * Fx`. The fixed-point operator would
        // round the MULTIPLIER into Q18.14 first, and these are authored decimals — 0.032
        // becomes 524/16384 = 0.031982, a 0.06 % error applied to a commander's 45,000-damage
        // death blast. Scaling the raw and rounding once is exact to the last representable
        // step, and it costs nothing because it happens at load.
        const auto scaled = static_cast<MagRaw>(
            std::llround(static_cast<double>(damage.raw()) * static_cast<double>(row.multiplier)));
        profile.addOverride(row.armor, sim::Mag::fromRaw(scaled));
    }

    return profile;
}

} // namespace rm::unitdef
