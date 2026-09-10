// The command line.
//
// §7 P7.5's stated test is "the suite covers what moved out", and this is where that has the
// most to say: argv parsing is pure — a `const char*[]` in, a struct out — and until P7.5 it was
// the only code in the engine that could not be tested at all, because it lived in an
// executable-only translation unit. Every case below is one that used to be checkable only by
// running the app and looking.
//
// NAMES DO NOT START WITH `--`. `catch_discover_tests` registers each case with CTest by passing
// its NAME as the filter, and Catch parses a leading `--` as an option — so a case called
// "--screenshot takes a path" passes when run by hand and fails through `ctest` with
// "Unrecognised token". Three of these did exactly that before being renamed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/Cli.hpp"
#include "app/View.hpp"

#include <filesystem>
#include <string>
#include <vector>

using Catch::Approx;
using rm::app::BenchOptions;
using rm::app::LookOptions;
using rm::app::MarchOptions;
using rm::app::ShotOptions;
using rm::app::WindowOptions;

namespace {

/// argv as the runtime hands it over: the program, then the MAP, then the flags.
///
/// A HELPER RATHER THAN LITERAL ARRAYS at each call, because the indexing is where these
/// functions go wrong and a test that got it wrong the same way would agree with the bug.
///
/// **`argv[1]` IS THE MAP**, and that is a real convention rather than padding: the map is
/// positional and several parsers therefore start their scan at index 2, not 1. Writing tests
/// without it made `--march` and `--look` look broken when they were reading a command line
/// that cannot occur.
class Args {
public:
    explicit Args(std::vector<std::string> words) : words_{std::move(words)} {
        pointers_.reserve(words_.size() + 2);
        pointers_.push_back("recoil-metal");
        pointers_.push_back("map.scmap");
        for (const std::string& word : words_) {
            pointers_.push_back(word.c_str());
        }
    }

    [[nodiscard]] int argc() const { return static_cast<int>(pointers_.size()); }
    [[nodiscard]] const char** argv() { return pointers_.data(); }

private:
    std::vector<std::string> words_;
    std::vector<const char*> pointers_;
};

} // namespace

TEST_CASE("FAF personality selects a supported template and rejects typos", "[ai-personality]") {
    Args defaults{{}};
    CHECK(rm::app::parseFafBaseTemplate(defaults.argc(), defaults.argv()) == "NormalMain");
    Args easy{{"--ai-personality", "easy"}};
    CHECK(rm::app::parseFafBaseTemplate(easy.argc(), easy.argv()) == "NormalMain");
    Args tech{{"--ai-personality", "tech"}};
    CHECK(rm::app::parseFafBaseTemplate(tech.argc(), tech.argv()) == "TechMain");
    for (const auto& [name, base] : std::vector<std::pair<std::string, std::string>>{
             {"medium", "ChallengeMain"}, {"rushland", "RushMainLand"},
             {"rushair", "RushMainAir"}, {"rushnaval", "RushMainNaval"},
             {"rushbalanced", "RushMainBalanced"}, {"turtle", "TurtleMain"},
             {"adaptive", "adaptive"}, {"random", "random"}}) {
        Args args{{"--ai-personality", name}};
        CHECK(rm::app::parseFafBaseTemplate(args.argc(), args.argv()) == base);
    }
    Args unknown{{"--ai-personality", "tehc"}};
    CHECK_THROWS(rm::app::parseFafBaseTemplate(unknown.argc(), unknown.argv()));
    Args missing{{"--ai-personality"}};
    CHECK_THROWS(rm::app::parseFafBaseTemplate(missing.argc(), missing.argv()));
}

TEST_CASE("a flag is found wherever it is, and only when it is there") {
    Args none{{}};
    CHECK_FALSE(rm::app::hasFlag(none.argc(), none.argv(), "--skirmish"));

    Args first{{"--skirmish"}};
    CHECK(rm::app::hasFlag(first.argc(), first.argv(), "--skirmish"));

    Args last{{"--skirmish"}};
    CHECK(rm::app::hasFlag(last.argc(), last.argv(), "--skirmish"));

    // NOT a prefix match: `--no-props` must not answer for `--props`, and a flag that is a
    // prefix of another is how a switch starts firing when its opposite is asked for.
    Args similar{{"--no-props"}};
    CHECK_FALSE(rm::app::hasFlag(similar.argc(), similar.argv(), "--props"));
}

TEST_CASE("logging flags select a level and optional file without swallowing flags") {
    Args configured{{"--log-level", "debug", "--log-file", "/tmp/recoil-metal.log"}};
    const rm::app::LoggingOptions logging =
        rm::app::parseLogging(configured.argc(), configured.argv());
    CHECK(logging.sink.level == rm::log::Level::Debug);
    CHECK(logging.sink.filePath == "/tmp/recoil-metal.log");
    CHECK(logging.problems.empty());

    Args invalid{{"--log-level", "verbose", "--log-file", "--skirmish"}};
    const rm::app::LoggingOptions refused =
        rm::app::parseLogging(invalid.argc(), invalid.argv());
    CHECK(refused.sink.level == rm::log::Level::Info);
    REQUIRE(refused.problems.size() == 2);
    CHECK(refused.problems[0].find("verbose") != std::string::npos);
    CHECK(refused.problems[1].find("--log-file") != std::string::npos);
}

TEST_CASE("window size is logical, bounded by the supported HUD floor, and fullscreen is explicit") {
    Args absent{{}};
    CHECK(rm::app::parseWindow(absent.argc(), absent.argv()).width == 1280);
    CHECK(rm::app::parseWindow(absent.argc(), absent.argv()).height == 720);
    CHECK_FALSE(rm::app::parseWindow(absent.argc(), absent.argv()).fullscreen);

    Args sized{{"--window", "2560", "1440"}};
    const WindowOptions large = rm::app::parseWindow(sized.argc(), sized.argv());
    CHECK(large.width == 2560);
    CHECK(large.height == 1440);

    Args bounded{{"--fullscreen", "--window", "800", "600"}};
    const WindowOptions floor = rm::app::parseWindow(bounded.argc(), bounded.argv());
    CHECK(floor.width == 1280);
    CHECK(floor.height == 720);
    CHECK(floor.fullscreen);
}

TEST_CASE("build preview acceptance opts into native events and requires a capture path") {
    Args preview{{"--build-preview-acceptance", "preview.png", "--backing", "2"}};
    const auto options = rm::app::parseWindow(preview.argc(), preview.argv());
    CHECK(options.buildPreviewAcceptance);
    CHECK(options.inputAcceptancePath == "preview.png");
    CHECK(options.simulatedBacking == 2.0f);
    Args missing{{"--build-preview-acceptance"}};
    CHECK_THROWS_AS(rm::app::parseWindow(missing.argc(), missing.argv()), std::invalid_argument);
}

TEST_CASE("a count is the value after its flag, and zero when there is none") {
    Args armies{{"--armies", "4"}};
    CHECK(rm::app::parseCount(armies.argc(), armies.argv(), "--armies") == 4);

    Args missing{{"--skirmish"}};
    CHECK(rm::app::parseCount(missing.argc(), missing.argv(), "--armies") == 0);

    // A flag at the very END with nothing after it. The scan must not read past argv, which is
    // the one way a parser like this crashes rather than misbehaving.
    Args dangling{{"--armies"}};
    CHECK(rm::app::parseCount(dangling.argc(), dangling.argv(), "--armies") == 0);
}

TEST_CASE("the capture selection type is the blueprint id after its flag") {
    Args engineer{{"--select-type", "UEL0105"}};
    CHECK(rm::app::parseSelectType(engineer.argc(), engineer.argv()) == "UEL0105");

    Args absent{{"--select", "1"}};
    CHECK(rm::app::parseSelectType(absent.argc(), absent.argv()).empty());

    Args dangling{{"--select-type"}};
    CHECK(rm::app::parseSelectType(dangling.argc(), dangling.argv()).empty());

    Args nextFlag{{"--select-type", "--screenshot", "/tmp/x.png"}};
    CHECK(rm::app::parseSelectType(nextFlag.argc(), nextFlag.argv()).empty());
}

TEST_CASE("the --screenshot flag takes a path and an optional size") {
    Args bare{{"--screenshot", "/tmp/x.png"}};
    const ShotOptions justPath = rm::app::parseShot(bare.argc(), bare.argv());
    CHECK(justPath.enabled);
    CHECK(justPath.path == "/tmp/x.png");
    CHECK(justPath.width > 0);   // a default rather than zero
    CHECK(justPath.height > 0);

    Args sized{{"--screenshot", "/tmp/y.png", "1280", "720"}};
    const ShotOptions withSize = rm::app::parseShot(sized.argc(), sized.argv());
    CHECK(withSize.enabled);
    CHECK(withSize.path == "/tmp/y.png");
    CHECK(withSize.width == 1280);
    CHECK(withSize.height == 720);

    Args absent{{"--skirmish"}};
    CHECK_FALSE(rm::app::parseShot(absent.argc(), absent.argv()).enabled);
}

TEST_CASE("the --march flag takes a place and a duration") {
    Args args{{"--march", "4096", "2048", "45"}};
    const MarchOptions march = rm::app::parseMarch(args.argc(), args.argv());
    CHECK(march.enabled);
    CHECK(march.x == Approx(4096.0f));
    CHECK(march.z == Approx(2048.0f));
    CHECK(march.seconds == Approx(45.0f));

    // `--play N` is the same pre-run without the blanket move order — the distinction that
    // makes a MATCH reproducible rather than a crowd walking at one point.
    Args play{{"--play", "60"}};
    const MarchOptions ran = rm::app::parseMarch(play.argc(), play.argv());
    CHECK(ran.enabled);
    CHECK(ran.seconds == Approx(60.0f));
    CHECK(ran.x == Approx(0.0f));
    CHECK(ran.z == Approx(0.0f));
}

TEST_CASE("native input acceptance requires a capture path and carries simulated backing") {
    Args args{{"--input-acceptance", "/tmp/input.png", "--backing", "2"}};
    const auto options = rm::app::parseWindow(args.argc(), args.argv());
    REQUIRE(options.inputAcceptancePath == "/tmp/input.png");
    REQUIRE(options.simulatedBacking == 2.0f);
    Args absent{{"--input-acceptance"}};
    REQUIRE_THROWS(rm::app::parseWindow(absent.argc(), absent.argv()));
    Args nextFlag{{"--input-acceptance", "--backing", "2"}};
    REQUIRE_THROWS(rm::app::parseWindow(nextFlag.argc(), nextFlag.argv()));
}

TEST_CASE("the --units flag takes a path, a count and a scale, in that order") {
    Args args{{"--units", "/units/UEL0201/UEL0201_unit.bp", "200", "1.5"}};
    const std::vector<rm::app::UnitOptions> units =
        rm::app::parseUnits(args.argc(), args.argv());
    REQUIRE(units.size() == 1);
    CHECK(units[0].modelPath == "/units/UEL0201/UEL0201_unit.bp");
    CHECK(units[0].count == 200);
    CHECK(units[0].scale == Approx(1.5f));

    // Repeated, because a scene may hold two kinds of unit — and the second must not overwrite
    // the first, which is what a single-struct parser would do.
    Args two{{"--units", "a.bp", "10", "--units", "b.bp", "20"}};
    const std::vector<rm::app::UnitOptions> both = rm::app::parseUnits(two.argc(), two.argv());
    REQUIRE(both.size() == 2);
    CHECK(both[0].modelPath == "a.bp");
    CHECK(both[0].count == 10);
    CHECK(both[1].modelPath == "b.bp");
    CHECK(both[1].count == 20);
}

TEST_CASE("the --units flag defaults its count and scale rather than zeroing them") {
    // A count of zero would be a scene with no units, which is never what "--units x.bp" means.
    Args args{{"--units", "x.bp"}};
    const std::vector<rm::app::UnitOptions> units =
        rm::app::parseUnits(args.argc(), args.argv());
    REQUIRE(units.size() == 1);
    CHECK(units[0].count > 0);
    CHECK(units[0].scale > 0.0f);
}

TEST_CASE("the --units flag does not swallow the next flag as its count") {
    // The trap in a positional parser: `--units x.bp --skirmish` must not read "--skirmish" as
    // a number. `atoi` would return 0 and produce a scene with no units and no error.
    Args args{{"--units", "x.bp", "--skirmish"}};
    const std::vector<rm::app::UnitOptions> units =
        rm::app::parseUnits(args.argc(), args.argv());
    REQUIRE(units.size() == 1);
    CHECK(units[0].count > 0);
}

TEST_CASE("the --bench flag and --bench-offscreen are told apart") {
    Args windowed{{"--bench", "600"}};
    const BenchOptions live = rm::app::parseBench(windowed.argc(), windowed.argv());
    CHECK(live.enabled);
    CHECK_FALSE(live.offscreen);
    CHECK(live.frames == 600);

    Args headless{{"--bench-offscreen", "300", "out.csv"}};
    const BenchOptions off = rm::app::parseBench(headless.argc(), headless.argv());
    CHECK(off.enabled);
    CHECK(off.offscreen);
    CHECK(off.frames == 300);

    Args neither{{"--skirmish"}};
    CHECK_FALSE(rm::app::parseBench(neither.argc(), neither.argv()).enabled);
}

TEST_CASE("the --look flag takes a place and a radius to frame it at") {
    // THREE values, not two — a place and how much of it to fit. Worth pinning, because two
    // reads as complete and the parser requires three: with only two it finds no `--look` at
    // all and the camera silently keeps its default framing.
    Args args{{"--look", "1024", "2048", "512"}};
    const LookOptions look = rm::app::parseLook(args.argc(), args.argv());
    CHECK(look.enabled);
    CHECK(look.x == Approx(1024.0f));
    CHECK(look.z == Approx(2048.0f));
    CHECK(look.radiusElmos == Approx(512.0f));

    Args tooFew{{"--look", "1024", "2048"}};
    CHECK_FALSE(rm::app::parseLook(tooFew.argc(), tooFew.argv()).enabled);

    Args absent{{"--skirmish"}};
    CHECK_FALSE(rm::app::parseLook(absent.argc(), absent.argv()).enabled);
}

TEST_CASE("the --focus flag is a distance in unit radii, zero when unasked") {
    Args args{{"--focus", "12"}};
    CHECK(rm::app::parseFocus(args.argc(), args.argv()) == Approx(12.0f));

    Args absent{{"--skirmish"}};
    CHECK(rm::app::parseFocus(absent.argc(), absent.argv()) == Approx(0.0f));
}

TEST_CASE("the --time flag is where in a clip a capture freezes") {
    Args args{{"--time", "0.35"}};
    CHECK(rm::app::parseAnimationTime(args.argc(), args.argv()) == Approx(0.35f));

    Args absent{{"--skirmish"}};
    CHECK(rm::app::parseAnimationTime(absent.argc(), absent.argv()) == Approx(0.0f));
}

TEST_CASE("the --ui flag selects one closed game-interface profile") {
    using rm::ui::GameProfile;

    Args absent{{"--skirmish"}};
    CHECK(rm::app::parseGameProfile(absent.argc(), absent.argv()) == GameProfile::Fa);

    for (const auto& [word, expected] :
         std::vector<std::pair<std::string, GameProfile>>{{"fa", GameProfile::Fa},
                                                          {"bar", GameProfile::Bar},
                                                          {"neutral", GameProfile::Neutral},
                                                          {"faf", GameProfile::ClassicFaf}}) {
        Args args{{"--ui", word}};
        CHECK(rm::app::parseGameProfile(args.argc(), args.argv()) == expected);
        CHECK(rm::ui::gameProfileName(expected) == word);
    }

    Args unknown{{"--ui", "future-game"}};
    CHECK(rm::app::parseGameProfile(unknown.argc(), unknown.argv()) == GameProfile::Fa);
}

TEST_CASE("game-interface profile parsing carries no process-global state") {
    Args classic{{"--ui", "faf"}};
    Args implicitFa{{}};

    CHECK(rm::app::parseGameProfile(classic.argc(), classic.argv())
          == rm::ui::GameProfile::ClassicFaf);
    CHECK(rm::app::parseGameProfile(implicitFa.argc(), implicitFa.argv())
          == rm::ui::GameProfile::Fa);
    CHECK(rm::app::parseGameProfile(classic.argc(), classic.argv())
          == rm::ui::GameProfile::ClassicFaf);
}

TEST_CASE("HUD effects are automatic until an explicit command-line override") {
    using rm::ui::EffectsLevel;

    Args absent{{"--skirmish"}};
    const rm::ui::EffectsPreference automatic =
        rm::app::parseUiEffects(absent.argc(), absent.argv());
    CHECK(automatic.level == EffectsLevel::Full);
    CHECK_FALSE(automatic.explicitOverride);
    CHECK(rm::ui::resolveEffects(automatic, false) == EffectsLevel::Full);
    CHECK(rm::ui::resolveEffects(automatic, true) == EffectsLevel::Off);

    for (const auto& [word, expected] :
         std::vector<std::pair<std::string, EffectsLevel>>{{"full", EffectsLevel::Full},
                                                           {"reduced", EffectsLevel::Reduced},
                                                           {"off", EffectsLevel::Off}}) {
        Args args{{"--ui-effects", word}};
        const rm::ui::EffectsPreference parsed =
            rm::app::parseUiEffects(args.argc(), args.argv());
        CHECK(parsed.level == expected);
        CHECK(parsed.explicitOverride);
        CHECK(rm::ui::resolveEffects(parsed, true) == expected);
        CHECK(rm::ui::effectsLevelName(expected) == word);
    }
}

TEST_CASE("the --data-dir flag collects every root it is given") {
    // Repeated flags ACCUMULATE rather than overwrite: order is priority, since an asset search
    // tries each root in turn.
    //
    // REAL DIRECTORIES, and that is not padding. `AssetSearch::addRoot` canonicalises and
    // silently drops anything that does not exist, which is right for a user typo and made the
    // first version of this test assert 0 == 1 while the parser was working perfectly. `/tmp`
    // and the current directory both exist on any machine this runs on.
    const std::string here = std::filesystem::current_path().string();

    Args one{{"--data-dir", "/tmp"}};
    CHECK(rm::app::parseAssetSearch(one.argc(), one.argv()).rootCount() == 1);

    Args two{{"--data-dir", "/tmp", "--data-dir", here}};
    CHECK(rm::app::parseAssetSearch(two.argc(), two.argv()).rootCount() == 2);

    // A root that does not exist is dropped rather than kept and failed on later.
    Args missing{{"--data-dir", "/definitely/not/a/directory/here"}};
    CHECK(rm::app::parseAssetSearch(missing.argc(), missing.argv()).rootCount() == 0);

    // And nothing given is no roots at all, not a default one — a search with a root nobody
    // asked for resolves paths against a directory the user never named.
    Args none{{}};
    CHECK(rm::app::parseAssetSearch(none.argc(), none.argv()).rootCount() == 0);
}

TEST_CASE("an empty command line parses to nothing enabled") {
    // The degenerate case, and the one every parser here has to agree about: no flags means no
    // modes, not a mode with default arguments.
    Args none{{}};
    CHECK_FALSE(rm::app::parseShot(none.argc(), none.argv()).enabled);
    CHECK_FALSE(rm::app::parseMarch(none.argc(), none.argv()).enabled);
    CHECK_FALSE(rm::app::parseBench(none.argc(), none.argv()).enabled);
    CHECK_FALSE(rm::app::parseLook(none.argc(), none.argv()).enabled);
    CHECK(rm::app::parseUnits(none.argc(), none.argv()).empty());
    CHECK(rm::app::parseFocus(none.argc(), none.argv()) == Approx(0.0f));
}

TEST_CASE("the factions flag parses a seat list, skips what it cannot name") {
    const char* argv[] = {"app", "--factions", "uef,Seraphim,CYBRAN"};
    const auto seats = rm::app::parseFactions(3, argv);
    REQUIRE(seats.size() == 3);
    CHECK(seats[0] == rm::sim::Faction::Uef);
    CHECK(seats[1] == rm::sim::Faction::Seraphim);  // case-insensitive, as factionFromName is
    CHECK(seats[2] == rm::sim::Faction::Cybran);

    // An unknown name is reported and SKIPPED — seating the wrong faction would be worse
    // than seating one fewer — and the names around it survive.
    const char* mixed[] = {"app", "--factions", "aeon,klingon,uef"};
    const auto partial = rm::app::parseFactions(3, mixed);
    REQUIRE(partial.size() == 2);
    CHECK(partial[0] == rm::sim::Faction::Aeon);
    CHECK(partial[1] == rm::sim::Faction::Uef);

    // Absent, or nothing parseable: empty, which the caller reads as "round-robin stays".
    const char* none[] = {"app", "--skirmish"};
    CHECK(rm::app::parseFactions(2, none).empty());
    const char* junk[] = {"app", "--factions", "romulan"};
    CHECK(rm::app::parseFactions(3, junk).empty());
}

TEST_CASE("archive mount order keeps name order with retail overrides last") {
    // Pure paths, no mounting: the question is only the sequence.
    const std::vector<std::filesystem::path> unordered{
        "mohodata.scd", "ambience.scd", "lua.scd", "units.scd",
    };
    const auto ordered = rm::app::orderArchivesForMount(unordered);
    REQUIRE(ordered.size() == 4);
    // Name order, except lua.scd mounts after mohodata.scd: its game Lua must
    // shadow the Moho stubs, and the last mount wins.
    CHECK(ordered[0].filename() == "ambience.scd");
    CHECK(ordered[1].filename() == "mohodata.scd");
    CHECK(ordered[2].filename() == "units.scd");
    CHECK(ordered[3].filename() == "lua.scd");
}
