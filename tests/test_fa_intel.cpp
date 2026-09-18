// Player-perspective coverage for intel claims (see docs/fa-exe-analysis-plan.md).
//
// What one side knows about another: the counter-intel flag filter (C-277), the
// jammer's deception (C-278), sonar's detect-without-identify (C-279), omni's
// detect-without-identify (C-280), allied sharing (C-281), the energy-brownout
// intel blackout (C-284), and the identification latch (C-285). The grid mechanics
// themselves live in test_intel.cpp; these cases pin the parts of the claims
// that were implemented but unproven, and each names the claim it answers.
#include "core/sim/Intel.hpp"

#include "core/sim/Army.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/Terrain.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using rm::sim::Army;
using rm::sim::Fx;
using rm::sim::Intel;
using rm::sim::IntelKind;
using rm::sim::UnitCatalog;
using rm::sim::UnitStore;

namespace {

/// A definition that sees `vision` elmos and nothing else, owned by the caller.
[[nodiscard]] rm::unitdef::UnitDef seer(float vision, float radar = 0.0f,
                                        float sonar = 0.0f, float omni = 0.0f) {
    rm::unitdef::UnitDef def;
    def.visionRadiusElmos = vision;
    def.radarRadiusElmos = radar;
    def.sonarRadiusElmos = sonar;
    def.omniRadiusElmos = omni;
    return def;
}

/// Two armies, allied or not.
[[nodiscard]] std::vector<Army> twoArmies(bool allied) {
    std::vector<Army> armies(2);
    armies[0].index = 0;
    armies[0].alliance = 0;
    armies[1].index = 1;
    armies[1].alliance = allied ? 0 : 1;
    return armies;
}

/// Puts a unit of `type` belonging to `army` at (x, z).
rm::sim::UnitId place(UnitStore& store, rm::UnitTypeIndex type, int army, float x,
                      float z) {
    UnitStore::Spawn spawn;
    spawn.type = type;
    spawn.transform.x = rm::sim::fxFromFloat(x);
    spawn.transform.z = rm::sim::fxFromFloat(z);
    spawn.motion.armyIndex = army;
    spawn.health.current = rm::sim::magFromFloat(100.0f);
    spawn.health.maximum = spawn.health.current;
    return store.spawn(spawn);
}

/// One alliance's view of a scene: everything it knows about, this tick.
[[nodiscard]] std::vector<rm::sim::Contact> seenBy(int alliance, const UnitStore& store,
                                                   const UnitCatalog& catalog,
                                                   std::span<const Army> armies,
                                                   const Intel& intel) {
    std::vector<rm::sim::Contact> contacts;
    rm::sim::contactsFor(alliance, store, catalog, armies, intel, 0, contacts);
    return contacts;
}

/// A flat field of `squares` squares, every corner at `height` elmos.
[[nodiscard]] rm::HeightField flatField(int squares, float height) {
    rm::HeightField field;
    field.squaresX = squares;
    field.squaresZ = squares;
    field.baseHeight = height;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

} // namespace

// --- C-277: counter-intel is a flag filter after coverage ---------------------
//
// The audit (FA-INTEL.md) records omni-bypass, self radar-stealth, cloak and the
// radar stealth field as pinned, and names the gaps: `sonarStealth` and the
// `SonarField` grid exist but were untested. These two cases close that gap.

TEST_CASE("C-277: a sonar-stealthed hull is absent from sonar", "[fa-intel]") {
    // The flag filter's sonar rung: `sonarStealth` clears the sonar bit exactly
    // the way `radarStealth` clears radar — the hull is inside the listener's
    // coverage and still unheard, not merely fainter.
    UnitCatalog catalog;
    static const rm::unitdef::UnitDef kEar = seer(0.0f, 0.0f, 400.0f);
    static const rm::unitdef::UnitDef kQuiet = [] {
        rm::unitdef::UnitDef def;
        def.sonarStealth = true;
        return def;
    }();
    static const rm::unitdef::UnitDef kLoud;
    const rm::UnitTypeIndex ear = catalog.add(&kEar);
    const rm::UnitTypeIndex quiet = catalog.add(&kQuiet);
    const rm::UnitTypeIndex loud = catalog.add(&kLoud);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, ear, 0, 200.0f, 200.0f);
    const rm::sim::UnitId hidden = place(store, quiet, 1, 500.0f, 200.0f);
    const rm::sim::UnitId heard = place(store, loud, 1, 600.0f, 200.0f);
    // Sonar only ever answers about naval hulls (C-279's layer byte); both
    // candidates are submersibles running submerged.
    store.motion()[hidden.index].submersible = true;
    store.motion()[hidden.index].submerged = true;
    store.motion()[heard.index].submersible = true;
    store.motion()[heard.index].submerged = true;

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    const auto contacts = seenBy(0, store, catalog, armies, intel);
    const auto knows = [&](rm::sim::UnitId id) {
        return std::ranges::any_of(contacts,
                                   [&](const rm::sim::Contact& c) { return c.unit == id; });
    };
    CHECK_FALSE(knows(hidden));
    REQUIRE(knows(heard));
    // And what is heard is a return, not an identification — sonar's half of
    // C-279, checked at the same table.
    for (const rm::sim::Contact& contact : contacts) {
        if (contact.unit == heard) {
            CHECK(contact.kind == rm::sim::ContactKind::Sonar);
        }
    }
}

TEST_CASE("C-277: a sonar stealth field hides its neighbours from sonar", "[fa-intel]") {
    // The field half of the same claim: `SonarStealthField` projects a "hidden
    // here" grid over the OWNER's alliance, and a hull inside it is absent from
    // hostile sonar while a hull outside it is an ordinary return.
    rm::unitdef::UnitDef listening = seer(0.0f, 0.0f, 800.0f);
    rm::unitdef::UnitDef generator;
    generator.sonarStealthFieldRadiusElmos = 100.0f;
    rm::unitdef::UnitDef plain;

    UnitCatalog catalog;
    const rm::UnitTypeIndex ear = catalog.add(&listening);
    const rm::UnitTypeIndex field = catalog.add(&generator);
    const rm::UnitTypeIndex hull = catalog.add(&plain);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, ear, 0, 200.0f, 200.0f);
    (void)place(store, field, 1, 600.0f, 600.0f);
    const rm::sim::UnitId inside = place(store, hull, 1, 640.0f, 600.0f);
    const rm::sim::UnitId outside = place(store, hull, 1, 900.0f, 600.0f);
    for (const rm::sim::UnitId id : {inside, outside}) {
        store.motion()[id.index].submersible = true;
        store.motion()[id.index].submerged = true;
    }

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    const auto contacts = seenBy(0, store, catalog, armies, intel);
    const auto knows = [&](rm::sim::UnitId id) {
        return std::ranges::any_of(contacts,
                                   [&](const rm::sim::Contact& c) { return c.unit == id; });
    };
    CHECK_FALSE(knows(inside));
    CHECK(knows(outside));
}

// --- C-278: jammer fakes -------------------------------------------------------

TEST_CASE("C-278: a jammer's false blips track the carrier", "[fa-intel]") {
    // The claim's observable half: fakes are fixed offsets from the JAMMER, not
    // autonomous contacts — when the carrier moves, every lie moves with it.
    // (The offset model itself diverges: ours is a fixed angle at the full
    // `jamRadius` plus shared drift, where retail draws a uniform magnitude in
    // `JamRadius[min,max]` — recorded in the coverage report.)
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    rm::unitdef::UnitDef deceiver;
    deceiver.jamRadiusElmos = 208.0f;  // the retail 26 ogrids
    deceiver.jammerBlips = 10;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex jammer = catalog.add(&deceiver);

    Intel intel;
    intel.configure(2, Fx::fromInt(4096), Fx::fromInt(4096),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId carrier = place(store, jammer, 1, 700.0f, 500.0f);
    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    const auto before = seenBy(0, store, catalog, armies, intel);
    std::size_t carrierContacts = 0;
    for (const rm::sim::Contact& contact : before) {
        if (contact.unit == carrier) {
            ++carrierContacts;
        }
    }
    REQUIRE(carrierContacts == 11);  // the real blip plus `jammerBlips` lies

    // The carrier walks 100 elmos east, staying inside the 800-elmo radar.
    store.transforms()[carrier.index].x += Fx::fromInt(100);
    intel.update(store, catalog, armies, nullptr);

    const auto after = seenBy(0, store, catalog, armies, intel);
    REQUIRE(after.size() == before.size());
    // Every contact keyed to the carrier — real and fake alike — shifted by
    // exactly the carrier's own displacement. A fake that stayed behind would
    // be a contact with a life of its own, which is the thing the claim denies.
    for (const rm::sim::Contact& moved : after) {
        if (moved.unit != carrier) {
            continue;
        }
        const auto match = std::ranges::find_if(before, [&](const rm::sim::Contact& old) {
            return old.unit == carrier && moved.x - old.x == Fx::fromInt(100)
                   && moved.z == old.z;
        });
        CHECK(match != before.end());
    }
}

TEST_CASE("C-278: a radar-stealthed jammer still scatters its lies", "[fa-intel]") {
    // The strongest version of the trick, and the shipped XES0102's loadout:
    // the carrier shows NOTHING itself — no real blip — while its fakes still
    // paint on the hostile scope. Fakes are emitted whenever the viewer's radar
    // covers the carrier's ground, whether or not the carrier resolved.
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    rm::unitdef::UnitDef deceiver;
    deceiver.radarStealth = true;
    deceiver.jamRadiusElmos = 208.0f;
    deceiver.jammerBlips = 10;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex jammer = catalog.add(&deceiver);

    Intel intel;
    intel.configure(2, Fx::fromInt(4096), Fx::fromInt(4096),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId carrier = place(store, jammer, 1, 700.0f, 500.0f);
    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    const auto contacts = seenBy(0, store, catalog, armies, intel);
    std::size_t fakes = 0;
    for (const rm::sim::Contact& contact : contacts) {
        if (contact.unit == carrier) {
            ++fakes;
            CHECK(contact.kind == rm::sim::ContactKind::Radar);
        }
    }
    // Exactly `jammerBlips` lies and no true return: the carrier's own position
    // is the one thing the scope does NOT show.
    CHECK(fakes == 10);
}

// --- C-279 + C-285: sonar detects, never identifies; the latch needs sight -----

TEST_CASE("C-279/C-285: sonar hears a hull but never identifies it", "[fa-intel]") {
    // C-279's detect/identify split, made observable through C-285's latch:
    // `seenEver` is set only by a Seen contact, so a hull known only by sonar
    // stays unidentified — and once vision HAS identified it, diving does not
    // take the identification back.
    rm::unitdef::UnitDef watching = seer(150.0f, 0.0f, 400.0f);
    rm::unitdef::UnitDef hull;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex sub = catalog.add(&hull);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 200.0f, 200.0f);
    const rm::sim::UnitId boat = place(store, sub, 1, 300.0f, 200.0f);
    store.motion()[boat.index].submersible = true;
    store.motion()[boat.index].submerged = true;

    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    // Inside vision AND sonar coverage, the submerged hull is a sonar return
    // only — and the identification latch stays clear.
    CHECK(rm::sim::contactKindForUnit(0, boat.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Sonar);
    CHECK_FALSE(intel.hasSeenEver(0, boat));

    // Surfaced inside the same vision radius, it is identified.
    store.motion()[boat.index].submerged = false;
    intel.update(store, catalog, armies, nullptr);
    CHECK(rm::sim::contactKindForUnit(0, boat.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Seen);
    CHECK(intel.hasSeenEver(0, boat));

    // And back under: sonar again, but the latch holds — identification
    // persists after losing line-of-sight (C-285's LOSEver half).
    store.motion()[boat.index].submerged = true;
    intel.update(store, catalog, armies, nullptr);
    CHECK(rm::sim::contactKindForUnit(0, boat.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Sonar);
    CHECK(intel.hasSeenEver(0, boat));
}

// --- C-281: allied intel sharing ----------------------------------------------

TEST_CASE("C-281: an ally's radar contact is shared as a blip, not a sighting",
          "[fa-intel]") {
    // The sharing claim's observable contract: an army with no sensors of its
    // own still gets what its ally's radar hears — as a BLIP, with the same
    // uncertainty the owner's scope shows. Sharing does not upgrade a return
    // into an identification.
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    rm::unitdef::UnitDef quiet;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex enemy = catalog.add(&quiet);

    Intel intel;
    intel.configure(2, Fx::fromInt(4096), Fx::fromInt(4096),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    // Army 0 fields nothing at all; army 1 — its ally — owns the radar.
    (void)place(store, watcher, 1, 200.0f, 200.0f);
    const rm::sim::UnitId target = place(store, enemy, 2, 400.0f, 200.0f);

    std::vector<Army> armies(3);
    armies[0].index = 0;
    armies[0].alliance = 0;
    armies[1].index = 1;
    armies[1].alliance = 0;  // allied with army 0
    armies[2].index = 2;
    armies[2].alliance = 1;  // the enemy

    intel.update(store, catalog, armies, nullptr);

    const auto contacts = seenBy(0, store, catalog, armies, intel);
    // Two contacts: the ally's own watcher (always Seen — allied units are
    // known exactly) and the enemy, which arrives as a BLIP with the same
    // uncertainty the owner's scope shows. Sharing does not upgrade a return
    // into an identification.

    REQUIRE(contacts.size() == 2);
    const auto blip = std::ranges::find_if(
        contacts, [&](const rm::sim::Contact& c) { return c.unit == target; });
    REQUIRE(blip != contacts.end());
    CHECK(blip->kind == rm::sim::ContactKind::Radar);
    CHECK(blip->isBlip());
    // The shared blip carries the same position error the owner's would — not
    // the true coordinate, and not a different lie.
    CHECK(blip->x != Fx::fromInt(400));
    CHECK_FALSE(intel.hasSeenEver(0, target));
}

// --- C-285: the identification latch -------------------------------------------

TEST_CASE("C-285: visual identification latches through losing sight", "[fa-intel]") {
    // `LOSEver` is set only on a LOSNow moment and then persists: radar alone
    // never sets it, and losing vision afterwards never clears it. This is the
    // flag C-158's acquisition priorities read.
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    rm::unitdef::UnitDef scout = seer(150.0f);
    rm::unitdef::UnitDef quiet;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex scoutType = catalog.add(&scout);
    const rm::UnitTypeIndex enemy = catalog.add(&quiet);

    Intel intel;
    intel.configure(2, Fx::fromInt(4096), Fx::fromInt(4096),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 200.0f, 200.0f);
    const rm::sim::UnitId observer = place(store, scoutType, 0, 400.0f, 300.0f);
    const rm::sim::UnitId target = place(store, enemy, 1, 400.0f, 200.0f);
    const std::vector<Army> armies = twoArmies(false);

    intel.update(store, catalog, armies, nullptr);
    CHECK(intel.hasSeenEver(0, target));

    // The scout walks away; radar still covers the target, so the contact
    // degrades to a blip — but the latch holds.
    store.transforms()[observer.index].z = Fx::fromInt(900);
    intel.update(store, catalog, armies, nullptr);
    CHECK(rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Radar);
    CHECK(intel.hasSeenEver(0, target));
}

// --- C-113: per-sense grid resolutions ------------------------------------------

TEST_CASE("C-113: omni stamps on the coarse grid like radar and sonar", "[fa-intel]") {
    // The audit's named gap: the omni-at-scale-4 fix was in code with no test
    // pinning a per-kind mip level. Vision is the fine grid (mip 1, 16-elmo
    // squares); radar, sonar and omni are the coarse one (mip 2, 32-elmo
    // squares) — C-077's scale-2/scale-4 split in this engine's units.
    Intel intel;
    intel.configure(1, Fx::fromInt(512), Fx::fromInt(512),
                    rm::sim::VisionStyle::ForgedAlliance);

    CHECK(intel.grid(0, IntelKind::Vision).squareElmos() == Fx::fromInt(16));
    CHECK(intel.grid(0, IntelKind::Radar).squareElmos() == Fx::fromInt(32));
    CHECK(intel.grid(0, IntelKind::Sonar).squareElmos() == Fx::fromInt(32));
    CHECK(intel.grid(0, IntelKind::Omni).squareElmos() == Fx::fromInt(32));
}

// --- C-007 + C-285 through the match tick ---------------------------------------

TEST_CASE("C-007/C-285: a scout walking into view turns a blip into a sighting",
          "[fa-intel]") {
    // The player-facing end of the whole system, run through `tickSkirmish`
    // rather than the pass directly: a contact that was a radar return becomes
    // a Seen contact — identified — once a friendly unit walks vision over it.
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    rm::unitdef::UnitDef scout = seer(150.0f);
    rm::unitdef::UnitDef quiet;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex scoutType = catalog.add(&scout);
    const rm::UnitTypeIndex enemy = catalog.add(&quiet);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 200.0f, 200.0f);
    const rm::sim::UnitId observer = place(store, scoutType, 0, 200.0f, 600.0f);
    const rm::sim::UnitId target = place(store, enemy, 1, 400.0f, 200.0f);
    std::vector<Army> armies = twoArmies(false);
    std::vector<rm::sim::Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);
    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .commandersEver = commandersEver,
                         .intel = &intel};
    const rm::sim::Terrain terrain{flatField(128, 0.0f)};

    (void)rm::sim::tickSkirmish(store, catalog, match, terrain);
    REQUIRE(rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
            == rm::sim::ContactKind::Radar);
    REQUIRE_FALSE(intel.hasSeenEver(0, target));

    // The scout is ordered north toward the contact. `orderTo` is the same
    // entry a routed move ends in; the tick's own movement stage walks it.
    rm::sim::MoveState& motion = store.motion()[observer.index];
    motion = rm::sim::defaultMotion(rm::sim::TickRate{});
    motion.armyIndex = 0;
    rm::sim::orderTo(motion, terrain, Fx::fromInt(400), Fx::fromInt(300));

    // 27 elmos a second at the default 10 Hz is 2.7 a tick; 300 elmos of travel
    // is well inside 200 ticks even with the turn.
    for (int i = 0; i < 200 && !intel.hasSeenEver(0, target); ++i) {
        (void)rm::sim::tickSkirmish(store, catalog, match, terrain);
    }

    CHECK(intel.hasSeenEver(0, target));
    CHECK(rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Seen);
}

// --- C-278: jammer offset distribution ------------------------------------------

TEST_CASE("C-278: jammer fakes draw a uniform magnitude inside JamRadius[Min,Max]",
          "[fa-intel]") {
    // The claim's distribution half: each fake's offset is a random direction AND a
    // uniform magnitude in `JamRadius[min,max]` — not the fixed full-radius ring the
    // first implementation stamped. The wander is peeled off exactly: a fake's drawn
    // offset is FIXED while its blip drift moves with the tick, so matching a contact
    // across two ticks against each ghost's own drift recovers the offset with no
    // tolerance at all.
    rm::unitdef::UnitDef watching = seer(0.0f, 800.0f);
    rm::unitdef::UnitDef deceiver;
    deceiver.jamRadiusMinElmos = 80.0f;
    deceiver.jamRadiusElmos = 208.0f;
    deceiver.jammerBlips = 10;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex jammer = catalog.add(&deceiver);

    Intel intel;
    intel.configure(2, Fx::fromInt(4096), Fx::fromInt(4096),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 500.0f, 500.0f);
    const rm::sim::UnitId carrier = place(store, jammer, 1, 700.0f, 500.0f);
    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    std::vector<rm::sim::Contact> atZero;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 0, atZero);
    std::vector<rm::sim::Contact> atFive;
    rm::sim::contactsFor(0, store, catalog, armies, intel, 5, atFive);

    const Fx carrierX = store.transforms()[carrier.index].x;
    const Fx carrierZ = store.transforms()[carrier.index].z;
    const Fx minR = rm::sim::fxFromFloat(80.0f);
    const Fx maxR = rm::sim::fxFromFloat(208.0f);

    // For each ghost identity — the real blip (offset 0) plus one per fake — predict
    // where a tick-0 contact must land at tick 5 under that ghost's drift. An exact
    // match identifies the ghost; the tick-0 contact minus the ghost's tick-0 drift
    // is then the drawn offset, in whole elmos with no tolerance to argue about.
    std::vector<Fx> magnitudes;
    for (int blip = 0; blip <= deceiver.jammerBlips; ++blip) {
        const rm::sim::UnitId ghost{
            carrier.index,
            static_cast<rm::Generation>(carrier.generation
                                        + static_cast<rm::Generation>(blip))};
        const auto [w0x, w0z] = rm::sim::radarBlipPosition(ghost, Fx{}, Fx{}, 0);
        const auto [w5x, w5z] = rm::sim::radarBlipPosition(ghost, Fx{}, Fx{}, 5);
        for (const rm::sim::Contact& contact : atZero) {
            if (contact.unit != carrier) {
                continue;
            }
            const Fx predictedX = contact.x - w0x + w5x;
            const Fx predictedZ = contact.z - w0z + w5z;
            const auto moved = std::ranges::find_if(
                atFive, [&](const rm::sim::Contact& later) {
                    return later.unit == carrier && later.x == predictedX
                           && later.z == predictedZ;
                });
            if (moved == atFive.end()) {
                continue;
            }
            const Fx offX = contact.x - w0x - carrierX;
            const Fx offZ = contact.z - w0z - carrierZ;
            magnitudes.push_back(rm::sim::fxHypot(offX, offZ));
            break;  // one ghost, one contact
        }
    }

    // The real blip plus ten fakes: eleven offsets, exactly one of them zero.
    REQUIRE(magnitudes.size() == 11);
    std::vector<Fx> fakes;
    for (const Fx magnitude : magnitudes) {
        if (magnitude > Fx{}) {
            fakes.push_back(magnitude);
        }
    }
    REQUIRE(fakes.size() == 10);

    // Uniform in [min, max], not a ring: the old fixed-magnitude model put all ten at
    // exactly `maxR`. Distinct magnitudes inside the range are the claim.
    for (const Fx magnitude : fakes) {
        CHECK(magnitude >= minR);
        CHECK(magnitude <= maxR);
    }
    const auto [flo, fhi] = std::ranges::minmax(fakes);
    CHECK(flo < fhi);
}

// --- C-280: omni detects, never identifies ---------------------------------------

TEST_CASE("C-280: an omni-only contact is a blip, never an identification",
          "[fa-intel]") {
    // Omni bypasses every counter-intel flag but does not set `LOSEver`: a unit known
    // ONLY through omni is a radar-class blip — detected, targetable at the worst
    // rank, unidentified — and the `seenEver` latch stays clear until real vision
    // does the identifying.
    rm::unitdef::UnitDef watching = seer(0.0f, 0.0f, 0.0f, 400.0f);
    rm::unitdef::UnitDef scout = seer(150.0f);
    rm::unitdef::UnitDef quiet;
    rm::unitdef::UnitDef sneaky;
    sneaky.cloak = true;  // omni ignores it — and vision can never identify it either

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex scoutType = catalog.add(&scout);
    const rm::UnitTypeIndex enemy = catalog.add(&quiet);
    const rm::UnitTypeIndex cloaked = catalog.add(&sneaky);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 200.0f, 200.0f);
    const rm::sim::UnitId observer = place(store, scoutType, 0, 200.0f, 900.0f);
    const rm::sim::UnitId target = place(store, enemy, 1, 300.0f, 200.0f);
    const rm::sim::UnitId ghost = place(store, cloaked, 1, 310.0f, 200.0f);
    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    // Omni covers both: detected — but as blips, and neither latch is set. Cloak does
    // not even keep omni out; it only keeps the contact unidentified forever.
    CHECK(rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Radar);
    CHECK(rm::sim::contactKindForUnit(0, ghost.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Radar);
    CHECK_FALSE(intel.hasSeenEver(0, target));
    CHECK_FALSE(intel.hasSeenEver(0, ghost));

    // The scout walks vision over both: the plain target is NOW identified and stays
    // identified — while the cloaked one remains an omni blip, because cloak still
    // defeats the eye that would have named it.
    store.transforms()[observer.index].z = Fx::fromInt(300);
    intel.update(store, catalog, armies, nullptr);
    CHECK(rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Seen);
    CHECK(intel.hasSeenEver(0, target));
    CHECK(rm::sim::contactKindForUnit(0, ghost.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Radar);
    CHECK_FALSE(intel.hasSeenEver(0, ghost));
}

// --- C-277: cloak is anti-vision only ---------------------------------------------

TEST_CASE("C-277: cloak hides from vision but not from radar", "[fa-intel]") {
    // The claim's split, side by side in one scene: a cloaked enemy inside vision AND
    // radar coverage is a radar blip — retail's filter clears `LOSNow` on self-cloak
    // and leaves the radar bit standing. (The sibling case in test_intel.cpp pinned
    // the wrong semantics for a while; this is the claim-level pair.)
    rm::unitdef::UnitDef watching = seer(400.0f, 400.0f);
    rm::unitdef::UnitDef sneaky;
    sneaky.cloak = true;
    rm::unitdef::UnitDef plain;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex cloaked = catalog.add(&sneaky);
    const rm::UnitTypeIndex visible = catalog.add(&plain);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 200.0f, 200.0f);
    const rm::sim::UnitId ghost = place(store, cloaked, 1, 300.0f, 200.0f);
    const rm::sim::UnitId seen = place(store, visible, 1, 400.0f, 200.0f);
    const std::vector<Army> armies = twoArmies(false);
    intel.update(store, catalog, armies, nullptr);

    // Same coverage, different flag: the plain unit is identified, the cloaked one is
    // a blip — present on the scope, absent from the eye.
    CHECK(rm::sim::contactKindForUnit(0, seen.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Seen);
    CHECK(rm::sim::contactKindForUnit(0, ghost.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Radar);
    CHECK_FALSE(intel.hasSeenEver(0, ghost));
}

// --- C-284: energy brownout disables intel -----------------------------------------

TEST_CASE("C-284: an energy brownout blacks out intel until recovery holds",
          "[fa-intel]") {
    // Retail's `IntelWatchThread`: when a unit's consumed ratio collapses, its intel
    // stops detecting — immediately — and stays off until the ratio has held for
    // `Intel.ReactivateTime` (10s at the default 10 Hz: 100 updates). A relapse
    // restarts the count. The watcher carries energy upkeep so its intel is a real
    // consumer; a zero-upkeep unit would never brown out.
    rm::unitdef::UnitDef watching = seer(400.0f, 400.0f);
    watching.upkeepEnergyPerSecond = 10.0f;
    rm::unitdef::UnitDef quiet;

    UnitCatalog catalog;
    const rm::UnitTypeIndex watcher = catalog.add(&watching);
    const rm::UnitTypeIndex enemy = catalog.add(&quiet);

    Intel intel;
    intel.configure(2, Fx::fromInt(1024), Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);

    UnitStore store;
    (void)place(store, watcher, 0, 200.0f, 200.0f);
    const rm::sim::UnitId target = place(store, enemy, 1, 300.0f, 200.0f);
    const std::vector<Army> armies = twoArmies(false);

    // Both economies funded in full: the watcher sees its enemy.
    std::vector<rm::sim::Economy> economies(2);
    const rm::sim::TickRate rate{};
    intel.update(store, catalog, armies, nullptr, rate, economies);
    CHECK(rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Seen);

    // Army 0's energy consumer is granted nothing — the brownout. Its intel dies the
    // same update: the enemy vanishes from the contact list entirely.
    economies[0].singleResourceFunded = rm::sim::kFxZero;
    economies[0].multiResourceFunded = rm::sim::kFxZero;
    intel.update(store, catalog, armies, nullptr, rate, economies);
    CHECK_FALSE(
        rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
            .has_value());

    // Power returns — but intel does NOT. The ratio must hold for the reactivate
    // window first; one tick short of it the scope is still dark.
    economies[0].singleResourceFunded = rm::sim::kFxOne;
    economies[0].multiResourceFunded = rm::sim::kFxOne;
    const auto reactivate = rate.ticks(rm::sim::Seconds{rm::sim::kIntelReactivateSeconds});
    for (rm::TickCount i = 0; i + 1 < reactivate; ++i) {
        intel.update(store, catalog, armies, nullptr, rate, economies);
    }
    CHECK_FALSE(
        rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
            .has_value());

    // A relapse inside the window restarts the count: brown out once more, recover
    // again, and the full window is owed a second time.
    economies[0].singleResourceFunded = rm::sim::kFxZero;
    economies[0].multiResourceFunded = rm::sim::kFxZero;
    intel.update(store, catalog, armies, nullptr, rate, economies);
    economies[0].singleResourceFunded = rm::sim::kFxOne;
    economies[0].multiResourceFunded = rm::sim::kFxOne;
    for (rm::TickCount i = 0; i + 1 < reactivate; ++i) {
        intel.update(store, catalog, armies, nullptr, rate, economies);
    }
    CHECK_FALSE(
        rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
            .has_value());

    // The hundredth consecutive funded update brings the senses back.
    intel.update(store, catalog, armies, nullptr, rate, economies);
    CHECK(rm::sim::contactKindForUnit(0, target.index, store, catalog, armies, intel)
          == rm::sim::ContactKind::Seen);
}
