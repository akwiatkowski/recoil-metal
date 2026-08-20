# recoil-metal — single-file build, sections in dependency order.
cmake_minimum_required(VERSION 3.25)
project(recoil-metal VERSION 0.1.0 LANGUAGES CXX C OBJCXX)

# --- Language level --------------------------------------------------------
# C++23: the showcase goal. Apple clang accepts -std=c++23 from Xcode 16 /
# recent CLT; if a specific feature is missing, downgrade that usage, not the
# whole standard.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo) # symbols matter: we benchmark
endif()

# --- Warnings-as-errors ----------------------------------------------------
# Showcase rule: the build is the first linter. third_party headers are
# included as SYSTEM (below) so metal-cpp's own style can't fail our build.
add_library(rm_project_warnings INTERFACE)
target_compile_options(rm_project_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wsign-conversion -Wold-style-cast
    -Wunused -Wshadow)

# --- metal-cpp (Apple's official C++ Metal bindings, header-only) ----------
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/metal-cpp/Metal/Metal.hpp)
    message(FATAL_ERROR
        "metal-cpp not found. Clone it once:\n"
        "  git clone --depth 1 https://github.com/apple/metal-cpp third_party/metal-cpp")
endif()

# --- miniz (single-file ZIP support for .sdz archives) ----------------------
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.h)
    message(FATAL_ERROR
        "miniz not found. Download it once:\n"
        "  mkdir -p third_party/miniz && curl -L https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip | tar -xf - -C third_party/miniz")
endif()

find_library(FOUNDATION_FW Foundation REQUIRED)
find_library(APPKIT_FW AppKit REQUIRED)
find_library(METAL_FW Metal REQUIRED)
find_library(QUARTZCORE_FW QuartzCore REQUIRED)
# ImageIO: PNG encoding for --screenshot. Part of the OS, not a dependency.
find_library(IMAGEIO_FW ImageIO REQUIRED)
find_library(COREGRAPHICS_FW CoreGraphics REQUIRED)

add_library(metal_cpp INTERFACE)
target_include_directories(metal_cpp SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/metal-cpp)
target_link_libraries(metal_cpp INTERFACE
    ${FOUNDATION_FW} ${METAL_FW} ${QUARTZCORE_FW})

add_library(miniz INTERFACE)
target_include_directories(miniz SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/miniz)

# miniz is a single-file C library. Compile it as C in its own target so the
# project's strict C++ warnings do not fire inside third-party code.
add_library(miniz_impl STATIC ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(miniz_impl PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniz)

# --- rm_core: pure C++, no Metal, no AppKit --------------------------------
# Everything in here is unit-testable on any machine, even one without a GPU.
add_library(rm_core STATIC
    src/core/map/HeightField.cpp
    src/core/map/MapInfo.cpp
    src/core/map/Smt.cpp
    src/core/map/TileAtlas.cpp
    src/core/lua/LuaValue.cpp
    src/core/lua/LuaTable.cpp
    src/core/map/ProceduralField.cpp
    src/core/map/Smf.cpp
    src/core/map/Scmap.cpp
    src/core/blueprint/BlueprintMesh.cpp
    src/core/map/PropBlueprint.cpp
    src/core/map/ScenarioSave.cpp
    src/core/map/TerrainType.cpp
    src/core/mesh/TerrainMesh.cpp
    src/core/mesh/ChunkDraws.cpp
    src/core/camera/OrbitCamera.cpp
    src/core/camera/Frustum.cpp
    src/core/bench/FrameStats.cpp
    src/core/model/Model.cpp
    src/core/model/S3o.cpp
    src/core/model/Scm.cpp
    src/core/model/Sca.cpp
    src/core/model/Pose.cpp
    src/core/text/TextLayout.cpp
    src/core/ui/Hud.cpp
    src/core/texture/Dds.cpp
    src/core/scene/UnitIcons.cpp
    src/core/scene/UnitPlacement.cpp
    src/core/scene/UnitBatch.cpp
    src/core/scene/PropBatch.cpp
    src/core/scene/Picking.cpp
    src/core/scene/Selection.cpp
    src/core/scene/GroundDecals.cpp
    src/core/scene/Particles.cpp
    src/core/sim/Army.cpp
    src/core/sim/Command.cpp
    src/core/sim/Combat.cpp
    src/core/sim/BuildOrder.cpp
    src/core/sim/Economy.cpp
    src/core/sim/Movement.cpp
    src/core/sim/Pathfinding.cpp
    src/core/sim/IdPool.cpp
    src/core/sim/Fx.cpp
    src/core/sim/TickRate.cpp
    src/core/sim/Terrain.cpp
    src/core/sim/UnitStore.cpp
    src/core/sim/UnitCatalog.cpp
    src/core/sim/UnitCensus.cpp
    src/core/sim/Skirmish.cpp
    src/core/sim/StateHash.cpp
    src/core/sim/SlowUpdate.cpp
    src/core/sim/Replay.cpp
    src/core/unit/UnitBlueprint.cpp
    src/core/unit/Weapon.cpp
    src/core/unit/FaDuration.cpp
    src/core/unit/UnitDef.cpp
    src/core/unit/Role.cpp
    src/core/unit/BuildTree.cpp
    src/core/data/Roster.cpp
    src/core/data/Opening.cpp
    src/core/data/MoveDef.cpp
    src/core/vfs/AssetSearch.cpp
    src/core/vfs/Vfs.cpp
    src/core/settings/Settings.cpp)
target_include_directories(rm_core PUBLIC src)
target_link_libraries(rm_core PUBLIC rm_project_warnings miniz miniz_impl)

# --- recoil-metal: the app --------------------------------------------------
add_executable(recoil-metal
    src/main.mm
    src/platform/Window.mm
    src/render/Renderer.mm)
target_link_libraries(recoil-metal PRIVATE
    rm_core rm_project_warnings metal_cpp ${APPKIT_FW}
    ${IMAGEIO_FW} ${COREGRAPHICS_FW})
# ARC for the Objective-C side only. metal-cpp objects are C++ and unaffected
# (they are manually retained/released — see AGENT.md gotchas).
target_compile_options(recoil-metal PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

# --- Tests ------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

    add_executable(rm_tests
        tests/test_types.cpp
        tests/test_fx.cpp
        tests/test_tick_rate.cpp
        tests/test_tick_rate_invariance.cpp
        tests/test_terrain.cpp
        tests/test_id_pool.cpp
        tests/test_unit_store.cpp
        tests/test_unit_census.cpp
        tests/test_match_invariants.cpp
        tests/test_state_hash.cpp
        tests/test_replay.cpp
        tests/test_smf.cpp
        tests/test_height_field.cpp
        tests/test_terrain_mesh.cpp
        tests/test_chunk_draws.cpp
        tests/test_orbit_camera.cpp
        tests/test_frustum.cpp
        tests/test_map_info.cpp
        tests/test_lua_table.cpp
        tests/test_smt.cpp
        tests/test_tile_atlas.cpp
        tests/test_frame_stats.cpp
        tests/test_s3o.cpp
        tests/test_real_model.cpp
        tests/test_real_scm.cpp
        tests/test_pose.cpp
        tests/test_real_sca.cpp
        tests/test_text_layout.cpp
        tests/test_hud.cpp
        tests/test_dds.cpp
        tests/test_unit_icons.cpp
        tests/test_unit_placement.cpp
        tests/test_army.cpp
        tests/test_players.cpp
        tests/test_command.cpp
        tests/test_build_order.cpp
        tests/test_combat.cpp
        tests/test_economy.cpp
        tests/test_movement.cpp
        tests/test_picking.cpp
        tests/test_selection.cpp
        tests/test_skirmish.cpp
        tests/test_ground_decals.cpp
        tests/test_particles.cpp
        tests/test_prop_culling.cpp
        tests/test_pathfinding.cpp
        tests/test_unit_blueprint.cpp
        tests/test_unit_def.cpp
        tests/test_role.cpp
        tests/test_build_tree.cpp
        tests/test_roster.cpp
        tests/test_opening.cpp
        tests/test_move_def.cpp
        tests/test_fa_duration.cpp
        tests/test_slow_update.cpp
        tests/test_asset_search.cpp
        tests/test_vfs.cpp
        tests/test_real_vfs.cpp
        tests/test_real_unit_blueprints.cpp
        tests/test_real_unitdefs.cpp
        tests/test_team_colours.cpp
        tests/test_unit_batch.cpp
        tests/test_real_map.cpp
        tests/test_terrain_type.cpp
        tests/test_real_scmap.cpp
        tests/test_blueprint_mesh.cpp
        tests/test_prop_blueprint.cpp
        tests/test_settings.cpp
        tests/test_real_stratum_normals.cpp
        tests/test_scenario_save.cpp
        tests/support/SmfWriter.cpp
        tests/support/S3oWriter.cpp)
    # tests/ on the include path so fixtures resolve as "support/SmfWriter.hpp",
    # matching how src/ headers resolve as "core/...".
    target_include_directories(rm_tests PRIVATE tests)
    target_link_libraries(rm_tests PRIVATE rm_core Catch2::Catch2WithMain)

    include(Catch)
    catch_discover_tests(rm_tests)

    # --- Boundary checks --------------------------------------------------------
    #
    # Invariants a Catch2 test cannot reach, because what they constrain is
    # `src/main.mm` — an executable-only translation unit nothing can link against.
    # A grep is the only tool that sees it, so the grep is a test: registered here
    # rather than left as a script somebody remembers to run, because the bug it
    # guards against went unnoticed precisely by never being checked.
    add_test(NAME "no caller hand-rolls the sim tick order"
             COMMAND ${CMAKE_SOURCE_DIR}/tools/check_sim_boundary.sh ${CMAKE_SOURCE_DIR})

    # Two sims must be able to exist in one process — the divergence harness is built on
    # it, and it is the one thing Recoil's globals make impossible at any price.
    add_test(NAME "the sim holds no file-scope mutable state"
             COMMAND ${CMAKE_SOURCE_DIR}/tools/check_no_sim_globals.sh ${CMAKE_SOURCE_DIR})

    # The fixed-point layer must give the same bits at every optimisation level (P2.1). Not
    # something a unit test can check: a test runs at whichever level its own binary was
    # built with, so the probe is compiled several times and its output compared. Slower than
    # the other checks — five compiles — but it is the whole basis of the determinism claim.
    add_test(NAME "fixed point is identical at every optimisation level"
             COMMAND ${CMAKE_SOURCE_DIR}/tools/check_fx_optimisation.sh)

    # No duration may be written as a number of ticks (PLAN2.md §5.1). Registered now that
    # the two constants the rule was written against are gone — before that it would have
    # been a red suite describing a known state rather than a guard.
    add_test(NAME "no duration is written in ticks"
             COMMAND ${CMAKE_SOURCE_DIR}/tools/check_no_tick_literals.sh ${CMAKE_SOURCE_DIR})

    # The sim's arithmetic must be fixed point (PLAN2.md §5.2). REGISTERED at last: this was
    # deferred at P0 because the sim was entirely float, and a check describing a known-bad
    # state is not a guard. P2.2 made it true, and this is the commit that makes it enforced.
    #
    # It permits `constexpr float` constants and a named list of boundary files, each with its
    # reason in the script — content is authored in human units and the renderer draws in
    # floats, so confining the conversion is what makes the rest checkable at all.
    add_test(NAME "the sim's arithmetic is fixed point"
             COMMAND ${CMAKE_SOURCE_DIR}/tools/check_no_sim_floats.sh ${CMAKE_SOURCE_DIR})
endif()
