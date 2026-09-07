// The mixer and the cues: the engine's first sound, tested without a device.
//
// Everything here runs the audio path except the speaker: synthesis is deterministic by
// construction (seeded xorshift, no clock), and the mixer is plain arithmetic over a voice
// pool — which is exactly why it lives in core and not in the platform layer.
#include <catch2/catch_test_macros.hpp>

#include "core/audio/CueEvents.hpp"
#include "core/audio/Cues.hpp"
#include "core/audio/Mixer.hpp"
#include "core/audio/WeaponSounds.hpp"
#include "core/audio/Xgs.hpp"
#include "core/audio/Xsb.hpp"
#include "core/audio/Xwb.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/unit/UnitDef.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

/// The smallest sound bank the walk accepts: one wave bank, one simple cue "Shot" whose sound
/// names track 3 directly. Offsets follow the retail header layout (Xsb.cpp).
[[nodiscard]] std::vector<std::uint8_t> tinySoundBank() {
    std::vector<std::uint8_t> b(138 + 64 + 12 + 5 + 5, 0);
    const auto u16 = [&](std::size_t at, std::uint16_t v) { b[at] = v & 0xff; b[at + 1] = v >> 8; };
    const auto u32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = (v >> (8 * i)) & 0xff;
    };
    std::memcpy(b.data(), "SDBK", 4);
    u16(4, 43); u16(6, 43);
    b[18] = 1;             // platform
    u16(19, 1);            // simple cues
    u16(21, 0);            // complex cues
    u16(25, 1);            // total cues
    b[27] = 1;             // wave banks
    u16(28, 1);            // sounds
    u16(30, 5);            // cue name table length ("Shot\0")
    const std::uint32_t waveBankNames = 138, sounds = 202, simpleCues = 214, names = 219;
    u32(34, simpleCues); u32(38, 0xffffffffu); u32(42, names);
    u32(46, 0xffffffffu); u32(50, 0xffffffffu); u32(54, 0xffffffffu);
    u32(58, waveBankNames); u32(62, 0xffffffffu); u32(66, 0xffffffffu); u32(70, sounds);
    std::memcpy(b.data() + 74, "Tiny", 4);
    std::memcpy(b.data() + waveBankNames, "TinyWaves", 9);
    // Sound: flags 0 (one track), category, volume, pitch, priority, entry length; track 3, bank 0.
    b[sounds] = 0;
    u16(sounds + 1, 1);  // XACT category 1, for the category-index read
    u16(sounds + 7, 12); u16(sounds + 9, 3); b[sounds + 11] = 0;
    // Simple cue: flags, sound offset.
    b[simpleCues] = 0; u32(simpleCues + 1, sounds);
    std::memcpy(b.data() + names, "Shot", 4);
    return b;
}

/// A miniature global settings file: one capped category, one unlimited, and a single
/// distance→volume curve that ends in authored silence — every field the parser reads.
[[nodiscard]] std::vector<std::uint8_t> tinyGlobalSettings() {
    std::vector<std::uint8_t> b(160, 0);
    const auto u16 = [&](std::size_t at, std::uint16_t v) { b[at] = v & 0xff; b[at + 1] = v >> 8; };
    const auto u32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = (v >> (8 * i)) & 0xff;
    };
    const auto f32at = [&](std::size_t at, float v) { std::memcpy(b.data() + at, &v, 4); };
    std::memcpy(b.data(), "XGSF", 4);
    u16(4, 43); u16(6, 42);
    u16(19, 2);              // categories
    u32(37, 100);            // curve section
    u32(41, 140);            // curve section end
    u32(45, 60);             // category records
    // Two categories: "Capped" at 3 voices, "Open" unlimited.
    u32(60, 130); u16(64, 3); std::memcpy(b.data() + 130, "Capped", 6);
    u32(66, 137); u16(70, 0xffff); std::memcpy(b.data() + 137, "Open", 4);
    // One curve: (500, -1000), (2000, -4000), (3000, -9600) — silence at 3000 elmos.
    f32at(100, 500.0f); f32at(104, -1000.0f);
    f32at(108, 2000.0f); f32at(112, -4000.0f);
    f32at(116, 3000.0f); f32at(120, -9600.0f);
    return b;
}

} // namespace

TEST_CASE("cues are synthesised once, non-silent, and identical on every call") {
    const rm::audio::Cue& shot = rm::audio::shotCue();
    REQUIRE_FALSE(shot.samples.empty());
    float peak = 0.0f;
    for (const float sample : shot.samples) {
        peak = std::max(peak, std::abs(sample));
    }
    CHECK(peak > 0.1f);
    CHECK(peak <= 1.5f);  // the mixer's tanh knee handles the sum; a cue itself stays tame
    // The same static object every time — the mixer stores pointers, so this is the
    // lifetime contract, not an optimisation.
    CHECK(&shot == &rm::audio::shotCue());
}

TEST_CASE("the mixer attenuates with distance and pans with offset") {
    rm::audio::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setListener(0.0f, 0.0f, 600.0f);

    std::vector<float> nearOut(512 * 2);
    mixer.play(rm::audio::shotCue(), 0.0f, 0.0f);
    mixer.render(nearOut.data(), 512);

    rm::audio::Mixer farMixer;
    farMixer.setMasterGain(1.0f);
    farMixer.setListener(0.0f, 0.0f, 600.0f);
    std::vector<float> farOut(512 * 2);
    farMixer.play(rm::audio::shotCue(), 3000.0f, 0.0f);
    farMixer.render(farOut.data(), 512);

    float nearPeak = 0.0f;
    float farPeak = 0.0f;
    float farLeft = 0.0f;
    float farRight = 0.0f;
    for (std::size_t i = 0; i < 512; ++i) {
        nearPeak = std::max(nearPeak, std::abs(nearOut[i * 2]));
        farPeak = std::max(farPeak, std::abs(farOut[i * 2]));
        farLeft = std::max(farLeft, std::abs(farOut[i * 2]));
        farRight = std::max(farRight, std::abs(farOut[i * 2 + 1]));
    }
    CHECK(nearPeak > farPeak);       // closer is louder
    CHECK(farRight > farLeft);       // and a shot to the +x side leans into the right ear
}

TEST_CASE("a voice frees its slot when the cue ends, and mute is silence") {
    rm::audio::Mixer mixer;
    mixer.setListener(0.0f, 0.0f, 600.0f);
    mixer.play(rm::audio::chimeCue(), 0.0f, 0.0f);
    CHECK(mixer.activeVoices() == 1);

    // Render past the cue's whole length; the voice must have retired.
    const std::size_t frames = rm::audio::chimeCue().samples.size() + 64;
    std::vector<float> out(frames * 2);
    mixer.render(out.data(), frames);
    CHECK(mixer.activeVoices() == 0);

    mixer.setMasterGain(0.0f);
    mixer.play(rm::audio::chimeCue(), 0.0f, 0.0f);
    std::vector<float> muted(256 * 2, 1.0f);
    mixer.render(muted.data(), 256);
    for (const float sample : muted) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("events map to cues: a shot sounds, a move does not") {
    rm::audio::Mixer mixer;
    mixer.setListener(0.0f, 0.0f, 600.0f);
    const std::vector<rm::sim::Event> events{
        {.kind = rm::sim::EventKind::WeaponFired, .at = {}},
        {.kind = rm::sim::EventKind::UnitCreated, .at = {}},
        {.kind = rm::sim::EventKind::UnitDestroyed, .at = {}},
    };
    rm::audio::playForEvents(mixer, events);
    CHECK(mixer.activeVoices() == 2);  // the shot and the death; a creation is not a moment
}

TEST_CASE("a sound bank resolves cue names to wave-bank tracks") {
    const auto bank = rm::audio::parseSoundBank(tinySoundBank());
    REQUIRE(bank.has_value());
    CHECK(bank->name == "Tiny");
    REQUIRE(bank->waveBanks == std::vector<std::string>{"TinyWaves"});
    REQUIRE(bank->cues.contains("Shot"));
    REQUIRE(bank->cues.at("Shot").tracks.size() == 1);
    CHECK(bank->cues.at("Shot").tracks.front().entry == 3);
    CHECK(bank->cues.at("Shot").tracks.front().waveBank == 0);
    // The sound record's category rides along — an index into the global settings' table.
    CHECK(bank->cues.at("Shot").category == 1);
    CHECK(bank->cues.at("Shot").pitchMin == 0);
    CHECK(bank->cues.at("Shot").pitchMax == 0);
    // Not a sound bank: nothing, never a throw.
    CHECK_FALSE(rm::audio::parseSoundBank({1, 2, 3}).has_value());
    auto wrongMagic = tinySoundBank();
    wrongMagic[0] = 'X';
    CHECK_FALSE(rm::audio::parseSoundBank(wrongMagic).has_value());
    // Truncated after the header: cues come back empty rather than faulting.
    auto truncated = tinySoundBank();
    truncated.resize(140);
    const auto partial = rm::audio::parseSoundBank(truncated);
    REQUIRE(partial.has_value());
    CHECK(partial->cues.empty());
}

TEST_CASE("the mixer honours pitch rate, category limits and the cutoff distance") {
    // PITCH: the same cue at rate 2 reads twice as fast, so its voice retires in half the
    // frames — an audible detune without synthesising a second waveform to compare against.
    rm::audio::Mixer plain;
    plain.setListener(0.0f, 0.0f, 600.0f);
    plain.play(rm::audio::chimeCue(), 0.0f, 0.0f);
    rm::audio::Mixer detuned;
    detuned.setListener(0.0f, 0.0f, 600.0f);
    detuned.play(rm::audio::chimeCue(), 0.0f, 0.0f, 1.0f, 2.0f, rm::audio::kUnlimitedInstances);
    const std::size_t half = rm::audio::chimeCue().samples.size() / 2;
    std::vector<float> out(half * 2);
    plain.render(out.data(), half);
    detuned.render(out.data(), half);
    CHECK(plain.activeVoices() == 1);
    CHECK(detuned.activeVoices() == 0);  // done in half the samples

    // INSTANCE LIMITS: a category capped at two thins the third play, not the pool.
    rm::audio::Mixer limited;
    limited.setListener(0.0f, 0.0f, 600.0f);
    limited.setCategoryLimit(1, 2);
    for (int i = 0; i < 3; ++i) {
        limited.play(rm::audio::chimeCue(), 0.0f, 0.0f, 1.0f, 1.0f, 1);
    }
    CHECK(limited.activeVoices() == 2);
    // An unlimited category is untouched by a limit it never named.
    rm::audio::Mixer open;
    open.setListener(0.0f, 0.0f, 600.0f);
    open.setCategoryLimit(1, 2);
    for (int i = 0; i < 3; ++i) {
        open.play(rm::audio::chimeCue(), 0.0f, 0.0f, 1.0f, 1.0f, 9);
    }
    CHECK(open.activeVoices() == 3);

    // LOD CUTOFF: beyond the authored silence distance a play costs nothing at all.
    rm::audio::Mixer cutoff;
    cutoff.setListener(0.0f, 0.0f, 600.0f);
    cutoff.setCutoffElmos(3000.0f);
    cutoff.play(rm::audio::chimeCue(), 2900.0f, 0.0f);
    cutoff.play(rm::audio::chimeCue(), 3100.0f, 0.0f);
    CHECK(cutoff.activeVoices() == 1);
}

TEST_CASE("the global settings carry category limits and silence distances") {
    const auto settings = rm::audio::parseGlobalSettings(tinyGlobalSettings());
    REQUIRE(settings.has_value());
    REQUIRE(settings->categories.size() == 2);
    CHECK(settings->categories[0].name == "Capped");
    CHECK(settings->categories[0].maxInstances == 3);
    CHECK(settings->categories[1].name == "Open");
    CHECK(settings->categories[1].maxInstances == rm::audio::kUnlimitedInstances);
    CHECK(settings->maxInstancesOf("Capped") == 3);
    CHECK(settings->maxInstancesOf("Unknown") == rm::audio::kUnlimitedInstances);
    REQUIRE(settings->silenceDistances.size() == 1);
    CHECK(settings->silenceDistances.front() == 3000.0f);
    CHECK(settings->cutoffElmos() == 3000.0f);  // world-scale, so it is the cutoff
    CHECK_FALSE(rm::audio::parseGlobalSettings({1, 2, 3}).has_value());
}

TEST_CASE("the retail UEF weapon bank names the Gauss cannon's three takes", "[corpus]") {
    const std::filesystem::path sounds =
        "/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/sounds";
    if (!std::filesystem::exists(sounds / "UELWeapon.xsb")) SKIP("retail sounds unavailable");
    const auto bank = rm::audio::loadSoundBank(sounds / "UELWeapon.xsb");
    REQUIRE(bank.has_value());
    CHECK(bank->name == "UELWeapon");
    CHECK(bank->waveBanks == std::vector<std::string>{"UELWeapon"});
    CHECK(bank->cues.size() == 36);
    // Measured on the retail bytes: a weighted list of entries 6, 5 and 7, in that order.
    REQUIRE(bank->cues.contains("UEL0201_Cannon_Sgl"));
    std::vector<std::uint16_t> tracks;
    for (const auto& track : bank->cues.at("UEL0201_Cannon_Sgl").tracks) tracks.push_back(track.entry);
    CHECK(tracks == std::vector<std::uint16_t>{6, 5, 7});
    // A single-take cue (event type 4) and a two-take one (type 6 with two entries).
    REQUIRE(bank->cues.at("UEL0103_Mortar").tracks.size() == 1);
    CHECK(bank->cues.at("UEL0103_Mortar").tracks.front().entry == 16);
    REQUIRE(bank->cues.at("UEL0104_Railgun").tracks.size() == 2);
    // THE AUTHORED VARIATION, measured on the retail bytes: a machine gun detuned a
    // semitone either way, every cue naming the "Weapons" category (index 6, unlimited
    // instances in SupCom.xgs).
    REQUIRE(bank->cues.contains("UEL0001_MachineGun"));
    CHECK(bank->cues.at("UEL0001_MachineGun").pitchMin == -200);
    CHECK(bank->cues.at("UEL0001_MachineGun").pitchMax == 100);
    CHECK(bank->cues.at("UEL0001_MachineGun").category == 6);
    // Every resolved track exists in the wave bank.
    const auto waves = rm::audio::loadWaveBank(sounds / "UELWeapon.xwb");
    REQUIRE(waves.has_value());
    CHECK(waves->entries.size() == 43);
    for (const auto& [name, cue] : bank->cues) {
        for (const auto& track : cue.tracks) CHECK(track.entry < waves->entries.size());
    }

    // The weapon-sound resolver: a catalog type naming the cue plays one of its takes,
    // chosen by the shooter's handle; an unnamed weapon resolves to nothing.
    rm::sim::UnitCatalog catalog;
    std::deque<rm::unitdef::UnitDef> defs;
    rm::unitdef::UnitDef tank;
    tank.name = "UEL0201";
    rm::unitdef::Weapon gun;
    gun.label = "MainGun";
    gun.fireSound = rm::unitdef::Weapon::FireSound{"UELWeapon", "UEL0201_Cannon_Sgl"};
    tank.weapons.push_back(gun);
    rm::unitdef::Weapon silent;
    silent.label = "Quiet";
    tank.weapons.push_back(silent);
    defs.push_back(tank);
    (void)catalog.add(&defs.back(), rm::sim::TickRate{});
    rm::audio::WeaponSounds weapons(sounds);
    weapons.learn(catalog);
    CHECK(weapons.bankCount() == 1);
    CHECK(weapons.weaponCount() == 1);
    std::set<const rm::audio::Cue*> takes;
    for (std::uint32_t seed = 0; seed < 3; ++seed) {
        const rm::audio::Cue* cue = weapons.cueFor("UEL0201:MainGun", seed);
        REQUIRE(cue != nullptr);
        CHECK_FALSE(cue->samples.empty());
        takes.insert(cue);
    }
    CHECK(takes.size() == 3);
    CHECK(weapons.cueFor("UEL0201:MainGun", 3) == weapons.cueFor("UEL0201:MainGun", 0));
    CHECK(weapons.cueFor("UEL0201:Quiet", 0) == nullptr);
    CHECK(weapons.cueFor("XSL0001:Nothing", 0) == nullptr);
    // The take with its authored detune: within the bank's pitch range as a rate, tagged
    // with the cue's category, and stable for a given seed.
    const auto& cannon = bank->cues.at("UEL0201_Cannon_Sgl");
    const float lowRate = std::exp2(static_cast<float>(cannon.pitchMin) / 1200.0f);
    const float highRate = std::exp2(static_cast<float>(cannon.pitchMax) / 1200.0f);
    const float lo = std::min(lowRate, highRate);
    const float hi = std::max(lowRate, highRate);
    for (std::uint32_t seed = 0; seed < 16; ++seed) {
        const rm::audio::WeaponSounds::Take take = weapons.takeFor("UEL0201:MainGun", seed);
        REQUIRE(take.cue != nullptr);
        CHECK(take.rate >= lo - 0.0001f);
        CHECK(take.rate <= hi + 0.0001f);
        CHECK(take.category == 6);
        CHECK(weapons.takeFor("UEL0201:MainGun", seed).rate == take.rate);
    }
    // The authored mix the banks assume: categories with their instance limits, and a
    // world-scale cutoff in the range the retail weapon curves occupy.
    const rm::audio::GlobalSettings* xgs = weapons.globalSettings();
    if (xgs != nullptr) {
        CHECK(xgs->categories.size() == 39);
        CHECK(xgs->maxInstancesOf("Weapons") == rm::audio::kUnlimitedInstances);
        CHECK(xgs->maxInstancesOf("Global") == 24);
        CHECK(xgs->cutoffElmos() > 1000.0f);
        CHECK(xgs->cutoffElmos() < 7000.0f);
    }
    // A second learn with nothing new is a no-op.
    weapons.learn(catalog);
    CHECK(weapons.weaponCount() == 1);
}

TEST_CASE("the retail wave banks decode: PCM in, mixer-ready cues out", "[corpus]") {
    // Gated on the retail install like every [corpus] test — the banks are loose files
    // beside gamedata, not archive members.
    const std::filesystem::path bank{
        "/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/sounds/Explosions.xwb"};
    if (!std::filesystem::exists(bank)) {
        SKIP("no retail sounds at " + bank.string());
    }
    const std::optional<rm::audio::WaveBank> loaded = rm::audio::loadWaveBank(bank);
    REQUIRE(loaded.has_value());
    CHECK(loaded->name == "Explosions");
    // Thirteen booms in the retail bank; a count drift means the parse walked wrong.
    CHECK(loaded->entries.size() == 13);
    float peak = 0.0f;
    for (const float sample : loaded->entries.front().samples) {
        peak = std::max(peak, std::abs(sample));
    }
    CHECK(peak > 0.05f);   // decoded something audible
    CHECK(peak <= 1.0f);   // and PCM scaling did not blow past full scale
}
