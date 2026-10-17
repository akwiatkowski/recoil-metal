#include <catch2/catch_test_macros.hpp>

#include "core/ui/OverlayMode.hpp"

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
