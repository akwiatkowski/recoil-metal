#include <catch2/catch_test_macros.hpp>

#include "platform/KeyInput.hpp"

#include <array>
#include <utility>

TEST_CASE("a character becomes one semantic event without losing event state") {
    const rm::KeyModifiers modifiers{.shift = true, .command = false, .control = true};
    const std::optional<rm::KeyEvent> event =
        rm::keyEventForCharacter('a', rm::KeyPhase::Press, true, modifiers);

    REQUIRE(event.has_value());
    CHECK(event->key == rm::Key::A);
    CHECK(event->phase == rm::KeyPhase::Press);
    CHECK(event->repeat);
    CHECK(event->modifiers.shift);
    CHECK_FALSE(event->modifiers.command);
    CHECK(event->modifiers.control);

    const std::optional<rm::KeyEvent> release =
        rm::keyEventForCharacter('a', rm::KeyPhase::Release, false, {});
    REQUIRE(release.has_value());
    CHECK(release->phase == rm::KeyPhase::Release);
    CHECK_FALSE(release->repeat);
}

TEST_CASE("the semantic vocabulary contains exactly the keys the game binds") {
    constexpr std::array<std::pair<char, rm::Key>, 22> kBindings{{
        {'\x1b', rm::Key::Escape},
        {'a', rm::Key::A},           {'d', rm::Key::D},           {'e', rm::Key::E},
        {'f', rm::Key::F},           {'n', rm::Key::N},           {'o', rm::Key::O},
        {'p', rm::Key::P},           {'r', rm::Key::R},           {'s', rm::Key::S},
        {'w', rm::Key::W},           {' ', rm::Key::Space},       {'0', rm::Key::Digit0},
        {'1', rm::Key::Digit1},      {'2', rm::Key::Digit2},      {'3', rm::Key::Digit3},
        {'4', rm::Key::Digit4},      {'5', rm::Key::Digit5},      {'6', rm::Key::Digit6},
        {'7', rm::Key::Digit7},      {'8', rm::Key::Digit8},      {'9', rm::Key::Digit9},
    }};
    for (const auto& [character, key] : kBindings) {
        CHECK(rm::keyForCharacter(character) == key);
    }
    CHECK_FALSE(rm::keyForCharacter('x').has_value());
}

TEST_CASE("control-group digits keep their numeric identity") {
    CHECK(rm::digitForKey(rm::Key::Digit0) == 0);
    CHECK(rm::digitForKey(rm::Key::Digit4) == 4);
    CHECK(rm::digitForKey(rm::Key::Digit9) == 9);
    CHECK_FALSE(rm::digitForKey(rm::Key::A).has_value());
}
