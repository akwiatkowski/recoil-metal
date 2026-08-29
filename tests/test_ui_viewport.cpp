#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/ui/Viewport.hpp"

using Catch::Approx;

TEST_CASE("an unset UI viewport leaves renderer fallback available", "[ui][viewport]") {
    const rm::ui::UiViewport viewport;
    CHECK(viewport.hudExtent().width == 0.0f);
    CHECK(viewport.hudExtent().height == 0.0f);
}

TEST_CASE("one UI viewport keeps logical, HUD, and drawable spaces coherent",
          "[ui][viewport]") {
    const rm::ui::UiViewport viewport =
        rm::ui::UiViewport::full(2560.0f, 1440.0f, 2.0f);

    CHECK(viewport.hudScale() == Approx(2.0f));
    CHECK(viewport.hudExtent().width == Approx(1280.0f));
    CHECK(viewport.hudExtent().height == Approx(720.0f));
    CHECK(viewport.drawableExtent().width == Approx(5120.0f));
    CHECK(viewport.drawableExtent().height == Approx(2880.0f));
    CHECK(viewport.fontRasterScale() == Approx(4.0f));

    const std::array<float, 2> logical{1600.0f, 1000.0f};
    const std::array<float, 2> hud = viewport.toHud(logical);
    CHECK(hud[0] == Approx(800.0f));
    CHECK(hud[1] == Approx(500.0f));
    CHECK(viewport.toLogical(hud)[0] == Approx(logical[0]));
    CHECK(viewport.toLogical(hud)[1] == Approx(logical[1]));
}

TEST_CASE("1280-point Retina window and 2560-pixel capture derive the same UI",
           "[ui][viewport]") {
    // This common 2x pair lies below the automatic 2.5x cap, so it produces the same layout and
    // font raster size even though one obtains its pixels from backing scale and the other from
    // HUD magnification. Larger same-pixel pairs can diverge once that cap is reached.
    const rm::ui::UiViewport window =
        rm::ui::UiViewport::full(1280.0f, 720.0f, 2.0f);
    const rm::ui::UiViewport capture =
        rm::ui::UiViewport::full(2560.0f, 1440.0f, 1.0f);

    CHECK(window.drawableExtent().width == Approx(capture.drawableExtent().width));
    CHECK(window.drawableExtent().height == Approx(capture.drawableExtent().height));
    CHECK(window.hudExtent().width == Approx(capture.hudExtent().width));
    CHECK(window.hudExtent().height == Approx(capture.hudExtent().height));
    CHECK(window.fontRasterScale() == Approx(capture.fontRasterScale()));

    const std::array<float, 2> authored{320.0f, 180.0f};
    CHECK(window.toDrawable(authored)[0] == Approx(capture.toDrawable(authored)[0]));
    CHECK(window.toDrawable(authored)[1] == Approx(capture.toDrawable(authored)[1]));
}

TEST_CASE("safe content converts with the rest of the UI viewport", "[ui][viewport]") {
    const rm::ui::UiViewport viewport = rm::ui::UiViewport::withSafeContent(
        2560.0f, 1440.0f, 2.0f, {80.0f, 40.0f, 2400.0f, 1360.0f});

    const rm::ui::Rect safe = viewport.hudSafeContent();
    CHECK(safe.x == Approx(40.0f));
    CHECK(safe.y == Approx(20.0f));
    CHECK(safe.width == Approx(1200.0f));
    CHECK(safe.height == Approx(680.0f));
}

TEST_CASE("even a one-pixel capture preserves the complete authored viewport",
          "[ui][viewport]") {
    const rm::ui::UiViewport viewport = rm::ui::UiViewport::full(1.0f, 1.0f);
    const rm::ui::Extent hud = viewport.hudExtent();

    CHECK(viewport.hudScale() == Approx(1.0f / 1280.0f));
    CHECK(hud.width == Approx(1280.0f));
    CHECK(hud.height == Approx(1280.0f));
    const std::array<float, 2> drawable = viewport.toDrawable({hud.width, hud.height});
    CHECK(drawable[0] == Approx(1.0f));
    CHECK(drawable[1] == Approx(1.0f));
}
