cmake_minimum_required(VERSION 3.20)
project(backtester LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings — catch latent bugs early
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# ----------------------------------------------------------------
# GoogleTest (fetched automatically — no manual install needed)
# ----------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ----------------------------------------------------------------
# Core library
# ----------------------------------------------------------------
add_library(backtester_lib STATIC
    src/sim_book.cpp
    src/execution_sim.cpp
    src/pnl_tracker.cpp
    src/report.cpp
    src/parity_harness.cpp
    src/live_engine.cpp
)
target_include_directories(backtester_lib PUBLIC include)

# ----------------------------------------------------------------
# Main runner
# ----------------------------------------------------------------
add_executable(backtester main.cpp)
target_link_libraries(backtester PRIVATE backtester_lib)

# ----------------------------------------------------------------
# Synthetic data generator tool
# ----------------------------------------------------------------
add_executable(synth_gen tools/synth_gen.cpp)
target_include_directories(synth_gen PRIVATE include)

# ----------------------------------------------------------------
# Paper trade runner (LiveEngine + PaperOrderRouter + FileFeedHandler)
# ----------------------------------------------------------------
add_executable(paper_trade tools/paper_trade.cpp)
target_link_libraries(paper_trade PRIVATE backtester_lib)
target_include_directories(paper_trade PRIVATE include ${CMAKE_SOURCE_DIR})

# ----------------------------------------------------------------
# Live trader — two ways to provide the feed handler project:
#
# Option A: subdirectory (both repos build together, recommended)
#   cmake -DBT_BUILD_LIVE=ON -DFH_DIR=/path/to/feedhandler -S . -B build-live
#   cmake --build build-live --target live_trader
#
# Option B: pre-built lib (build feed handler first separately)
#   cmake -DBT_BUILD_LIVE=ON \
#         -DFH_INCLUDE_DIR=/path/to/feedhandler/include \
#         -DFH_LIB=/path/to/feedhandler/build/libfeedhandler.a \
#         -S . -B build-live
#   cmake --build build-live --target live_trader
#
# If BT_BUILD_LIVE=ON and neither path is set, auto-detects a sibling
# folder named "order book engine" (e.g. Desktop layout).
# ----------------------------------------------------------------
option(BT_BUILD_LIVE "Build live_trader (requires feed handler project)" OFF)
if(BT_BUILD_LIVE)
    if(NOT DEFINED FH_DIR AND NOT (DEFINED FH_INCLUDE_DIR AND DEFINED FH_LIB))
        set(_fh_sibling "${CMAKE_SOURCE_DIR}/../order book engine")
        if(EXISTS "${_fh_sibling}/CMakeLists.txt")
            get_filename_component(FH_DIR "${_fh_sibling}" ABSOLUTE)
            message(STATUS "BT_BUILD_LIVE: auto-detected feed handler at ${FH_DIR}")
        endif()
    endif()

    if(DEFINED FH_DIR)
        # Option A: pull in the feed handler as a subdirectory.
        # EXCLUDE_FROM_ALL: its targets build only when live_trader needs them.
        # Keep feedhandler's own tests/benches off unless the user asked for them.
        set(FH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(FH_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        add_subdirectory(${FH_DIR} _feedhandler EXCLUDE_FROM_ALL)
        # feedhandler CMakeLists defines target "feedhandler" → libfeedhandler.a
        set(_fh_includes ${FH_DIR}/include)
        set(_fh_lib feedhandler)
    elseif(DEFINED FH_INCLUDE_DIR AND DEFINED FH_LIB)
        # Option B: pre-built lib + include path.
        set(_fh_includes ${FH_INCLUDE_DIR})
        set(_fh_lib ${FH_LIB})
    else()
        message(FATAL_ERROR
            "BT_BUILD_LIVE=ON requires either:\n"
            "  -DFH_DIR=/path/to/feedhandler          (builds together)\n"
            "  -DFH_INCLUDE_DIR=... -DFH_LIB=...      (pre-built lib)\n"
            "  or a sibling folder named \"order book engine\"")
    endif()

    add_executable(live_trader tools/live_main.cpp)
    target_link_libraries(live_trader PRIVATE backtester_lib ${_fh_lib})
    target_include_directories(live_trader PRIVATE include ${CMAKE_SOURCE_DIR} ${_fh_includes})
endif()

# ----------------------------------------------------------------
# Tests
# ----------------------------------------------------------------
enable_testing()

add_executable(backtester_tests
    tests/test_event_queue.cpp
    tests/test_sim_book.cpp
    tests/test_execution_sim.cpp
    tests/test_queue_position.cpp
    tests/test_pnl.cpp
    tests/test_parity_harness.cpp
)
target_link_libraries(backtester_tests PRIVATE backtester_lib GTest::gtest_main)
target_include_directories(backtester_tests PRIVATE include)

include(GoogleTest)
gtest_discover_tests(backtester_tests)
