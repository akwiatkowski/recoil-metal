#include <catch2/catch_test_macros.hpp>

#include "core/ui/OverlayMode.hpp"

#include <optional>

TEST_CASE("toggling the military overlay leaves economy and team color alone",
          "[ui][overlay]") {
    rm::ui::OverlayState state;
    rm::ui::toggleOverlay(state, rm::ui::Overlay::Military);
    CHECK(rm::ui::overlaySet(state, rm::ui::Overlay::Military));
    CHECK_FALSE(rm::ui::overlaySet(state, rm::ui::Overlay::Economy));
    CHECK_FALSE(rm::ui::overlaySet(state, rm::ui::Overlay::TeamColor));
}

TEST_CASE("toggling one overlay twice restores the unchecked state",
          "[ui][overlay]") {
    rm::ui::OverlayState state;
    rm::ui::toggleOverlay(state, rm::ui::Overlay::Economy);
    rm::ui::toggleOverlay(state, rm::ui::Overlay::Economy);
    CHECK_FALSE(rm::ui::overlaySet(state, rm::ui::Overlay::Economy));
}

TEST_CASE("economy and team-color toggles are independent of each other",
          "[ui][overlay]") {
    rm::ui::OverlayState state;
    rm::ui::toggleOverlay(state, rm::ui::Overlay::TeamColor);
    CHECK(rm::ui::overlaySet(state, rm::ui::Overlay::TeamColor));
    CHECK_FALSE(rm::ui::overlaySet(state, rm::ui::Overlay::Economy));
    rm::ui::toggleOverlay(state, rm::ui::Overlay::Economy);
    CHECK(rm::ui::overlaySet(state, rm::ui::Overlay::TeamColor));
    CHECK(rm::ui::overlaySet(state, rm::ui::Overlay::Economy));
}
TEST_CASE("control-modified overlay keys resolve to the retail toggles",
          "[ui][overlay]") {
    // Retail `defaultKeyMap.lua:12-14`: Ctrl-W opens `tog_military`, Ctrl-E
    // `tog_defense` (unit color), Ctrl-R `tog_econ`. Plain E/R keep their
    // window/debug meanings, so the resolver keys off the control modifier.
    CHECK(rm::ui::overlayForKey(rm::Key::W, true)
          == std::optional<rm::ui::Overlay>{rm::ui::Overlay::Military});
    CHECK(rm::ui::overlayForKey(rm::Key::E, true)
          == std::optional<rm::ui::Overlay>{rm::ui::Overlay::TeamColor});
    CHECK(rm::ui::overlayForKey(rm::Key::R, true)
          == std::optional<rm::ui::Overlay>{rm::ui::Overlay::Economy});
    CHECK_FALSE(rm::ui::overlayForKey(rm::Key::W, false).has_value());
    CHECK_FALSE(rm::ui::overlayForKey(rm::Key::E, false).has_value());
    CHECK_FALSE(rm::ui::overlayForKey(rm::Key::R, false).has_value());
    CHECK_FALSE(rm::ui::overlayForKey(rm::Key::A, true).has_value());
}
TEST_CASE("the team-color overlay remaps icon tints by diplomacy", "[ui][overlay]") {
    // Retail's `teamcolor` multifunction button (`multifunction.lua`:
    // `TeamColorHandler` -> `TeamColorMode(calculateTeamColors())`): ON paints
    // units by diplomacy to the focus army — self Player blue, allies green,
    // enemies red, neutrals burlywood — not by per-army colour. OFF keeps each
    // army's own colour. Palette defaults from `teamColorSettings.colors`.
    const rm::TeamColour own{1.0f, 0.0f, 0.0f, 1.0f};
    const rm::ui::OverlayState on{.teamColor = true};
    const rm::ui::OverlayState off;
    // Focus army 0: 0 is self, 1 shares alliance 0, 2 is alliance 2, 3 neutral.
    CHECK(rm::ui::iconTintFor(on, 0, 0, 0, false, own)
          == rm::TeamColour{0x43 / 255.0f, 0x6e / 255.0f, 0xee / 255.0f, 1.0f});
    CHECK(rm::ui::iconTintFor(on, 0, 1, 0, false, own)
          == rm::TeamColour{0x40 / 255.0f, 0xbf / 255.0f, 0x40 / 255.0f, 1.0f});
    CHECK(rm::ui::iconTintFor(on, 0, 2, 2, false, own)
          == rm::TeamColour{0xe8 / 255.0f, 0x0a / 255.0f, 0x0a / 255.0f, 1.0f});
    CHECK(rm::ui::iconTintFor(on, 0, 3, 3, true, own)
          == rm::TeamColour{0xde / 255.0f, 0xb8 / 255.0f, 0x87 / 255.0f, 1.0f});
    CHECK(rm::ui::iconTintFor(off, 0, 2, 2, false, own) == own);
}
