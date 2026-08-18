# recoil-metal — single-file build, sections in dependency order.
cmake_minimum_required(VERSION 3.25)
project(recoil-metal VERSION 0.1.0 LANGUAGES CXX OBJCXX)

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
    src/core/map/ScenarioSave.cpp
    src/core/map/TerrainType.cpp
    src/core/mesh/TerrainMesh.cpp
    src/core/camera/OrbitCamera.cpp
    src/core/bench/FrameStats.cpp
    src/core/model/Model.cpp
    src/core/model/S3o.cpp
    src/core/model/Scm.cpp
    src/core/model/Sca.cpp
    src/core/model/Pose.cpp
    src/core/texture/Dds.cpp
    src/core/scene/UnitPlacement.cpp
    src/core/scene/UnitBatch.cpp)
target_include_directories(rm_core PUBLIC src)
target_link_libraries(rm_core PUBLIC rm_project_warnings)

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
        tests/test_smf.cpp
        tests/test_height_field.cpp
        tests/test_terrain_mesh.cpp
        tests/test_orbit_camera.cpp
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
        tests/test_dds.cpp
        tests/test_unit_placement.cpp
        tests/test_team_colours.cpp
        tests/test_unit_batch.cpp
        tests/test_real_map.cpp
        tests/test_terrain_type.cpp
        tests/test_real_scmap.cpp
        tests/test_scenario_save.cpp
        tests/support/SmfWriter.cpp
        tests/support/S3oWriter.cpp)
    # tests/ on the include path so fixtures resolve as "support/SmfWriter.hpp",
    # matching how src/ headers resolve as "core/...".
    target_include_directories(rm_tests PRIVATE tests)
    target_link_libraries(rm_tests PRIVATE rm_core Catch2::Catch2WithMain)

    include(Catch)
    catch_discover_tests(rm_tests)
endif()
