#include <catch2/catch_test_macros.hpp>

#include "core/ui/BuildPanel.hpp"
#include "core/ui/PanelPages.hpp"
#include "core/ui/Roster.hpp"

#include <array>

TEST_CASE("build pages belong to builder types", "[ui][panel-pages]") {
    rm::ui::PanelPages pages;
    pages.build(3) = 2;
    pages.build(7) = 4;

    CHECK(pages.build(3) == 2);
    CHECK(pages.build(7) == 4);
    CHECK(pages.build(5) == 0);
}

TEST_CASE("the roster page belongs to one exact ordered selection", "[ui][panel-pages]") {
    rm::ui::PanelPages pages;
    const std::array first{rm::sim::UnitId{2, 1}, rm::sim::UnitId{5, 1}};
    pages.showRoster(first);
    pages.roster() = 3;
    CHECK(pages.roster() == 3);

    const std::array reordered{first[1], first[0]};
    pages.showRoster(reordered);
    CHECK(pages.roster() == 0);
    pages.roster() = 2;

    const std::array reused{rm::sim::UnitId{5, 1}, rm::sim::UnitId{2, 2}};
    pages.showRoster(reused);
    CHECK(pages.roster() == 0);
    pages.roster() = 1;
    pages.showRoster({});
    CHECK(pages.roster() == 0);
}

TEST_CASE("input keeps the page of the roster last shown", "[ui][panel-pages]") {
    rm::ui::PanelPages pages;
    const std::array shown{rm::sim::UnitId{2, 1}, rm::sim::UnitId{5, 1}};
    pages.showRoster(shown);
    pages.roster() = 3;

    // A key may already have changed application selection, but input still addresses the old
    // on-screen tiles. Only the next display callback announces that the replacement was shown.
    const std::array next{rm::sim::UnitId{7, 1}};
    CHECK(pages.roster() == 3);
    pages.showRoster(next);
    CHECK(pages.roster() == 0);
}

TEST_CASE("command pages belong to game profiles", "[ui][panel-pages]") {
    rm::ui::PanelPages pages;
    pages.commands(rm::ui::GameProfile::Fa) = 2;
    pages.commands(rm::ui::GameProfile::Bar) = 4;

    CHECK(pages.commands(rm::ui::GameProfile::Fa) == 2);
    CHECK(pages.commands(rm::ui::GameProfile::Bar) == 4);
    CHECK(pages.commands(rm::ui::GameProfile::Neutral) == 0);
    CHECK(pages.commands(rm::ui::GameProfile::ClassicFaf) == 0);
}

TEST_CASE("panel layouts remain the page clamping authority", "[ui][panel-pages]") {
    rm::ui::PanelPages pages;
    const rm::ui::FrameLayout frame =
        rm::ui::frameLayout(rm::ui::UiViewport::full(1280.0f, 720.0f));

    pages.build(3) = 99;
    const rm::ui::BuildPanelLayout build = rm::ui::buildPanelLayout(frame, 1, pages.build(3));
    CHECK(build.page == 0);
    pages.build(3) = build.page;
    CHECK(pages.build(3) == 0);

    const std::array selection{rm::sim::UnitId{2, 1}};
    pages.showRoster(selection);
    pages.roster() = 99;
    const rm::ui::RosterLayout roster = rm::ui::rosterLayout(frame, 1, pages.roster());
    CHECK(roster.page == 0);
    pages.roster() = roster.page;
    CHECK(pages.roster() == 0);
}
