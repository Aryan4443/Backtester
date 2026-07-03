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
# Live trader (requires feed handler project headers + lib).
# Enable with: cmake -DBT_BUILD_LIVE=ON -DFH_INCLUDE_DIR=<path> -DFH_LIB=<path>
# ----------------------------------------------------------------
option(BT_BUILD_LIVE "Build live_trader (requires feed handler project)" OFF)
if(BT_BUILD_LIVE)
    if(NOT DEFINED FH_INCLUDE_DIR)
        message(FATAL_ERROR "BT_BUILD_LIVE=ON requires -DFH_INCLUDE_DIR=<feedhandler/include>")
    endif()
    if(NOT DEFINED FH_LIB)
        message(FATAL_ERROR "BT_BUILD_LIVE=ON requires -DFH_LIB=<path/to/libfeedhandler.a>")
    endif()
    add_executable(live_trader tools/live_main.cpp)
    target_link_libraries(live_trader PRIVATE backtester_lib ${FH_LIB})
    target_include_directories(live_trader PRIVATE include ${CMAKE_SOURCE_DIR} ${FH_INCLUDE_DIR})
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
