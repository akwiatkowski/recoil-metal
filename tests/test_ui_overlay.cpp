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
