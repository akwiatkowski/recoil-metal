// What Forged Alliance's `WaitSeconds` actually waits, and what that does to a burst weapon.
//
// THREE KINDS OF CASE. The table pins the transform against `simInit.lua` — that is the test
// PLAN2 §7 P3.5 names. The corpus cases check it against the real archive, which is the only
// thing that can catch the field being read from the wrong place or a value shape nobody
// anticipated. And the firing cases check the part that made the correction worth making: a
// burst weapon delivering its whole burst.
#include <catch2/catch_test_macros.hpp>

#include "core/unit/FaDuration.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/Weapon.hpp"
#include "core/lua/LuaTable.hpp"
#include "core/sim/Combat.hpp"

#include "support/TestRoster.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using rm::sim::seconds;
using rm::sim::TickRate;
using rm::unitdef::faWaitSeconds;
using rm::unitdef::Weapon;

namespace {

/// The extracted corpus, the same place every other real-blueprint test reads from. Assets are
/// never committed (AGENT.md rule 3), so these SKIP when it has not been extracted.
[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

[[nodiscard]] std::vector<std::filesystem::path> blueprints() {
    std::vector<std::filesystem::path> found;
    const std::filesystem::path root = unitRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        return found;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (entry.is_regular_file()
            && entry.path().filename().string().ends_with("_unit.bp")) {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

[[nodiscard]] std::string readAll(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

// --- The transform, which is §7 P3.5's stated test ---------------------------------------

TEST_CASE("WaitSeconds quantises to a tenth and adds one") {
    // The three cases the plan names, verbatim.
    CHECK(faWaitSeconds(seconds(0.05f)).value == 0.1f);
    CHECK(faWaitSeconds(seconds(0.2f)).value == 0.3f);
    CHECK(faWaitSeconds(seconds(1.0f)).value == 1.1f);
}

TEST_CASE("a tenth of a second is the floor, not the first quantum") {
    // FA's guard is `n <= 0.1`, so exactly 0.1 takes the ONE-tick branch and stays 0.1 rather
    // than becoming 0.2. An implementation using `<` would inflate the corpus's single most
    // common non-zero salvo delay — 23 of the 98 weapons that state one say 0.1.
    CHECK(faWaitSeconds(seconds(0.1f)).value == 0.1f);
    CHECK(faWaitSeconds(seconds(0.0f)).value == 0.1f);
    CHECK(faWaitSeconds(seconds(-1.0f)).value == 0.1f);
}

TEST_CASE("a duration that is not a multiple of a tenth floors before inflating") {
    // The two shapes the corpus actually contains: 0.25 (two weapons) and 0.33 (one). Both
    // round DOWN to the tenth and then gain a tick, which is what `floor(n * 10) + 1` does and
    // what a round-to-nearest reading would get wrong for the first of them.
    CHECK(faWaitSeconds(seconds(0.25f)).value == 0.3f);
    CHECK(faWaitSeconds(seconds(0.33f)).value == 0.4f);

    // And two larger ones, where the inflation is proportionally small but still a whole tick.
    CHECK(faWaitSeconds(seconds(1.5f)).value == 1.6f);
    CHECK(faWaitSeconds(seconds(2.3f)).value == 2.4f);
}

TEST_CASE("the correction is rate-independent") {
    // It is a fact about how FA's content was AUTHORED — a 10 Hz Lua coroutine — not about how
    // fast this sim ticks. So the corrected SECONDS are the same at every rate, and only the
    // tick count that follows differs. Getting this backwards would make the same blueprint
    // mean different things at different rates, which is what §5.1 exists to prevent.
    const rm::sim::Seconds corrected = faWaitSeconds(seconds(0.2f));
    CHECK(corrected.value == 0.3f);
    CHECK(TickRate{10}.ticks(corrected) == 3);
    CHECK(TickRate{20}.ticks(corrected) == 6);
    CHECK(TickRate{50}.ticks(corrected) == 15);
}

// --- What it does to a parsed weapon ------------------------------------------------------

namespace {

/// One weapon, from a blueprint fragment — the smallest thing that exercises the parse.
[[nodiscard]] Weapon parseOne(const std::string& fields) {
    // Braced, because `parseTable` reads the first table LITERAL — the same shape a `.bp`
    // presents as `UnitBlueprint { ... }`. Without the outer braces the first literal it finds
    // is the weapon array itself, under no key at all.
    const std::string source = "{ Weapon = { { " + fields + " } } }";
    const auto table = rm::lua::parseTable(source);
    REQUIRE(table.has_value());
    const rm::lua::Value* array = table->path("Weapon");
    REQUIRE(array != nullptr);
    std::vector<Weapon> weapons = rm::unitdef::weaponsFrom(*array);
    REQUIRE(weapons.size() == 1);
    return weapons.front();
}

} // namespace

TEST_CASE("a stated salvo delay is corrected at parse time") {
    const Weapon burst = parseOne("MuzzleSalvoSize = 3, MuzzleSalvoDelay = 0.2");
    CHECK(burst.burstSize == 3);
    CHECK(burst.burstDelay.value == 0.3f);
    CHECK(burst.bursts());
}

TEST_CASE("a salvo delay of zero stays zero") {
    // The game guards the wait with `if salvoDelay > 0` (`DefaultProjectileWeapon.lua:1130`), so
    // a stated zero means no wait happens rather than a wait of zero. Correcting it anyway
    // would hand 304 of the 402 weapons that state the field a 100 ms gap the game never gives
    // them — and, because `bursts()` needs a delay, would turn every multi-muzzle weapon in the
    // game into a slow burst.
    const Weapon simultaneous = parseOne("MuzzleSalvoSize = 4, MuzzleSalvoDelay = 0");
    CHECK(simultaneous.burstDelay.value == 0.0f);
    CHECK_FALSE(simultaneous.bursts());
    CHECK(simultaneous.burstSize == 4);
}

TEST_CASE("RateOfFire is not corrected") {
    // The counter-intuitive half of `11 §3.6`: `RateOfFire` is enforced by the ENGINE clock, so
    // it carries no `WaitSeconds` bias. A reading that "corrected" it would make every weapon
    // in the game 100 ms slow — 2 shots a second becoming 1.67.
    const Weapon gun = parseOne("RateOfFire = 2");
    CHECK(gun.rateOfFire == 2.0f);
    CHECK(gun.reloadTicks(TickRate{10}) == 5);
}

TEST_CASE("a weapon with no salvo fields is an ordinary one") {
    const Weapon plain = parseOne("RateOfFire = 1");
    CHECK(plain.burstSize == 1);
    CHECK_FALSE(plain.bursts());
}

TEST_CASE("DamageFriendly is retained for a death weapon") {
    const Weapon death = parseOne("WeaponCategory = 'Death', DamageFriendly = true");
    CHECK(death.damageFriendly);
}

// --- The corpus, which is what catches a misreading -------------------------------------

TEST_CASE("every stated salvo delay in the corpus survives the correction") {
    const std::vector<std::filesystem::path> files = blueprints();
    if (files.empty()) {
        SKIP("the FA corpus is not extracted at ~/projects/llm/input/faf/units");
    }

    std::size_t stated = 0;      // weapons stating MuzzleSalvoDelay at all
    std::size_t positive = 0;    // stating one above zero
    std::size_t bursting = 0;    // and a salvo size above one, so they really burst
    std::size_t inflated = 0;    // whose corrected value is a whole tick longer

    for (const std::filesystem::path& file : files) {
        const std::string source = readAll(file);
        const auto def = rm::unitbp::load(source, file.string());
        if (!def) {
            continue;
        }
        const auto raw = rm::lua::parseTable(source);
        const rm::lua::Value* array = raw ? raw->path("Weapon") : nullptr;
        if (array == nullptr) {
            continue;
        }

        for (std::size_t w = 0; w < def->weapons.size() && w < array->items.size(); ++w) {
            const std::optional<double> authored =
                array->items[w].numberAt("MuzzleSalvoDelay");
            if (!authored) {
                continue;
            }
            ++stated;

            const Weapon& weapon = def->weapons[w];
            if (*authored <= 0.0) {
                // The guard: a stated zero is left alone, which is the claim most easily got
                // wrong and the one that would change 304 weapons.
                CHECK(weapon.burstDelay.value == 0.0f);
                continue;
            }
            ++positive;
            if (weapon.bursts()) {
                ++bursting;
            }

            // The correction NEVER shortens a duration and never leaves it unchanged above the
            // floor. That is the whole content of "quantised and inflated", asserted over every
            // value in the archive rather than over the four in the table above.
            const auto delay = static_cast<double>(weapon.burstDelay.value);
            CHECK(delay >= *authored);
            if (*authored > 0.1) {
                CHECK(delay > *authored);
                ++inflated;
            }
        }
    }

    // The measured shape of the corpus, pinned so a content change or a parse regression is
    // visible as a number rather than as a silently smaller set. Counted over weapons rather
    // than over units, since a unit may carry several.
    CHECK(stated == 402);
    CHECK(positive == 98);
    CHECK(inflated == 74);  // the 98 less the 24 at or below the 0.1 floor
    CHECK(bursting == 94);
}

// --- The firing pass: a burst delivers its whole burst -----------------------------------

namespace {

/// A type carrying one gun, described by the three numbers a burst needs.
///
/// Turreted, so no case here is also a test of aiming: `canFireAt` would otherwise gate the
/// first shot on the hull coming round and every tick number below would be about turn rate.
[[nodiscard]] rm::unitdef::UnitDef gunnerDef(float rateOfFire, int burstSize, float burstDelay) {
    rm::unitdef::UnitDef def;
    def.name = "test_gunner";
    def.categories = {"LAND"};

    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = rm::unitdef::WeaponRole::DirectFire;
    weapon.targetPriorities = {{"LAND"}};
    weapon.damage = rm::sim::magFromFloat(10.0f);
    weapon.maxRange = rm::sim::fxFromFloat(300.0f);
    weapon.muzzleVelocityElmosPerSecond = 100.0f;
    weapon.rateOfFire = rateOfFire;
    weapon.turreted = true;
    weapon.burstSize = burstSize;
    weapon.burstDelay = seconds(burstDelay);
    def.weapons.push_back(weapon);
    return def;
}

[[nodiscard]] rm::unitdef::UnitDef targetDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_target";
    def.categories = {"LAND"};
    return def;
}

/// The ticks, within `ticks`, on which a lone gunner of this type got a shot away.
[[nodiscard]] std::vector<int> shotTicks(const rm::unitdef::UnitDef& shooter, int ticks) {
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    rm::test::Roster roster;
    (void)roster.add(roster.addType(shooter), 0.0f, 0.0f, 0, 100.0f);
    // Tough enough to survive the whole window: this measures cadence, not lethality.
    (void)roster.add(roster.addType(targetDef()), 0.0f, 100.0f, 1, 1.0e6f);

    std::vector<rm::sim::Projectile> shots;
    std::vector<int> fired;
    for (int tick = 1; tick <= ticks; ++tick) {
        const std::size_t n = rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots,
                                                  roster.rate);
        for (std::size_t s = 0; s < n; ++s) {
            fired.push_back(tick);
        }
    }
    return fired;
}

} // namespace

TEST_CASE("a burst weapon fires its whole burst before reloading") {
    // One shot a second with a three-shot burst 0.3 s apart, at the default 10 Hz. The burst
    // lands on ticks 1, 4 and 7 — three shots inside the first second — and only then does the
    // full ten-tick reload run, so the next burst opens on tick 17.
    //
    // The cycle is therefore 16 ticks for three shots, not 10 for one: 3 + 3 for the two gaps
    // inside the burst, then 10 for the reload after its last shot.
    const std::vector<int> fired = shotTicks(gunnerDef(1.0f, 3, 0.3f), 20);

    REQUIRE(fired.size() == 5);
    CHECK(fired[0] == 1);
    CHECK(fired[1] == 4);
    CHECK(fired[2] == 7);
    CHECK(fired[3] == 17);  // 7 + the ten-tick reload
    CHECK(fired[4] == 20);  // and the second burst is under way
}

TEST_CASE("a weapon that does not burst keeps exactly its old cadence") {
    // The regression guard on the change. `burstSize` of 1 must reduce to the single-shot path
    // that existed before, tick for tick — a burst mechanism that shifted every ordinary
    // weapon by one tick would move the whole game's balance while looking like a new feature.
    const std::vector<int> fired = shotTicks(gunnerDef(1.0f, 1, 0.0f), 25);

    REQUIRE(fired.size() == 3);
    CHECK(fired[0] == 1);
    CHECK(fired[1] == 11);
    CHECK(fired[2] == 21);
}

TEST_CASE("a salvo size with no delay is not a burst") {
    // 106 weapons state a size above one and no delay. Those fire their muzzles inside one
    // tick, which this engine models as the single shot it already fired — NOT as a burst
    // spread over 100 ms gaps. Treating them as bursts would silently stretch a third of the
    // armed corpus's firing patterns.
    const std::vector<int> fired = shotTicks(gunnerDef(1.0f, 4, 0.0f), 25);

    REQUIRE(fired.size() == 3);
    CHECK(fired[1] == 11);  // the plain reload, unchanged
}

TEST_CASE("the burst is what makes a burst weapon's damage right") {
    // The reason the correction is worth making, as a number — and it lands exactly on the
    // figure the report gives. Same rate of fire and same damage per shot; only the burst
    // differs. Over ten seconds the burst weapon's 16-tick cycle gets six full bursts away
    // plus two shots of a seventh, and the same weapon read as single-shot — which is what
    // this engine did before P3.5 — manages ten.
    //
    // Ten of twenty is **50% of its real output**, which is `11 §5`'s "every weapon's DPS is
    // wrong by up to 50% on burst weapons", reproduced rather than quoted.
    const std::size_t withBurst = shotTicks(gunnerDef(1.0f, 3, 0.3f), 100).size();
    const std::size_t withoutBurst = shotTicks(gunnerDef(1.0f, 1, 0.0f), 100).size();

    CHECK(withoutBurst == 10);  // one a second for ten seconds
    CHECK(withBurst == 20);     // six bursts of three, plus two of a seventh
    CHECK(withoutBurst * 2 == withBurst);
}

TEST_CASE("the correction lengthens a burst rather than shortening it") {
    // End to end: the same authored blueprint fields, read with and without the transform.
    // 0.2 s authored is 3 ticks corrected and 2 ticks uncorrected at 10 Hz, so an uncorrected
    // reading fires its burst 100 ms per shot too fast — which is the systematic error
    // `11 §2.1` describes, measured here in shots rather than in seconds.
    const std::vector<int> corrected = shotTicks(gunnerDef(1.0f, 3, 0.3f), 12);
    const std::vector<int> uncorrected = shotTicks(gunnerDef(1.0f, 3, 0.2f), 12);

    REQUIRE(corrected.size() == 3);
    REQUIRE(uncorrected.size() == 3);
    CHECK(corrected[2] == 7);
    CHECK(uncorrected[2] == 5);
}
