cmake_minimum_required(VERSION 3.20)

project(
    cppdi
    VERSION 1.0.0
    DESCRIPTION "A tiny, thread-safe, TCA-inspired dependency injection toolkit for modern C++"
    HOMEPAGE_URL "https://github.com/jaroshevskii/cpp-di-demo"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(CPPDI_BUILD_TESTS "Build and register the test suite" ON)
option(CPPDI_ENABLE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(CTest)
include(FetchContent)

include(cmake/CompileSettings.cmake)

# ---------------------------------------------------------------------------
# Core library: header-only
# ---------------------------------------------------------------------------
add_library(cppdi INTERFACE)
target_include_directories(
    cppdi
    INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(cppdi INTERFACE cxx_std_20)
set_target_properties(cppdi PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Example application: the dice CLI
# ---------------------------------------------------------------------------
add_library(dice_app INTERFACE)
target_include_directories(dice_app INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
target_link_libraries(dice_app INTERFACE cppdi)

add_executable(dice_cli examples/DiceCli.cpp)
target_link_libraries(dice_cli PRIVATE dice_app)
cppdi_setup_compile_settings(dice_cli)

# ---------------------------------------------------------------------------
# Tests (Catch2 v3, fetched on first configure)
# ---------------------------------------------------------------------------
if(CPPDI_BUILD_TESTS AND BUILD_TESTING)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(
        tests
        tests/DependenciesTests.cpp
        tests/LoggerTests.cpp
        tests/RandomTests.cpp
        tests/AppTests.cpp
        tests/DependencyTests.cpp
    )
    target_link_libraries(tests PRIVATE dice_app Catch2::Catch2WithMain)
    cppdi_setup_compile_settings(tests)

    include(Catch)
    catch_discover_tests(tests)
endif()

# ---------------------------------------------------------------------------
# Install + package config
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cppdi EXPORT cppdiTargets)
install(
    EXPORT cppdiTargets
    NAMESPACE cppdi::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppdi
)