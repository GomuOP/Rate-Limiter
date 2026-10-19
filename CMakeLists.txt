cmake_minimum_required(VERSION 3.24)
project(ratelimiter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(RL_WERROR "Treat warnings as errors (CI)" OFF)
option(RL_SANITIZE "Build with ASan + UBSan" OFF)
option(RL_TSAN "Build with ThreadSanitizer (exclusive with RL_SANITIZE)" OFF)
option(RL_BUILD_TESTS "Build the test target" ON)
option(RL_ENABLE_LTO "Enable IPO/LTO if supported" OFF)

# --- warning flags carried on an INTERFACE target our own code links -----------
add_library(rl_warnings INTERFACE)
target_compile_options(rl_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow)
if(RL_WERROR)
    target_compile_options(rl_warnings INTERFACE -Werror)
endif()

# --- sanitizers (debug preset) ----------------------------------------------
if(RL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# TSan instruments every memory access to find data races between threads. It
# replaces the allocator and shadow memory layout just as ASan does, so the two
# cannot share a binary: it gets its own preset (design 0.20).
if(RL_TSAN)
    if(RL_SANITIZE)
        message(FATAL_ERROR "RL_TSAN and RL_SANITIZE are mutually exclusive")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# --- LTO (release preset) -------------------------------------------------------
if(RL_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    if(_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(STATUS "IPO/LTO not supported: ${_ipo_msg}")
    endif()
endif()

# --- third-party dependencies -----------------------------------------------
include(FetchContent)
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE ALWAYS)
# hiredis still declares cmake_minimum_required(VERSION 3.4); keep newer CMake happy.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)

FetchContent_Declare(hiredis
    GIT_REPOSITORY https://github.com/redis/hiredis.git
    GIT_TAG        v1.2.0
    GIT_SHALLOW    TRUE)

set(DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HIREDIS_TEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json hiredis)

find_package(Threads REQUIRED)

# hiredis exports different target names across versions / build modes.
if(TARGET hiredis::hiredis)
    set(RL_HIREDIS hiredis::hiredis)
elseif(TARGET hiredis_static)
    set(RL_HIREDIS hiredis_static)
else()
    set(RL_HIREDIS hiredis)
endif()
# Vendored hiredis headers sit at the root of its source tree (<hiredis.h>),
# not under a hiredis/ prefix like a system install.
FetchContent_GetProperties(hiredis SOURCE_DIR RL_HIREDIS_SRC)

# --- core library ----------------------------------------------------------
add_library(rl_core STATIC
    src/core/errc.cpp
    src/core/config.cpp
    src/core/stats.cpp
    src/limit/token_bucket.cpp
    src/limit/sliding_window.cpp
    src/limit/sharded_limiter.cpp
    src/limit/key_extractor.cpp
    src/net/event_loop.cpp
    src/net/listener.cpp
    src/net/connection.cpp
    src/http/request_parser.cpp
    src/http/response.cpp
    src/http/query.cpp
    src/http/server.cpp
    src/redis/redis_client.cpp
    src/redis/redis_rate_limiter.cpp
)
target_include_directories(rl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# SYSTEM: hiredis' sds.h uses flexible array members that trip -Wpedantic.
target_include_directories(rl_core SYSTEM PRIVATE ${RL_HIREDIS_SRC})
target_link_libraries(rl_core
    PUBLIC  nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE rl_warnings ${RL_HIREDIS})

# embed the Lua script and the status page as raw string literals at configure time
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/redis/token_bucket.lua" RL_LUA_SCRIPT)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/redis/lua_script.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/rl/lua_script.inc"
    @ONLY)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/http/dashboard.html" RL_DASHBOARD_HTML)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/http/dashboard.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/rl/dashboard.inc"
    @ONLY)

target_include_directories(rl_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# --- executable ----------------------------------------------------------------
add_executable(ratelimiter src/main.cpp)
target_link_libraries(ratelimiter PRIVATE rl_core rl_warnings)

# --- tests -------------------------------------------------------------------
if(RL_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)

    enable_testing()
    add_executable(rl_tests
        tests/limit/test_token_bucket.cpp
        tests/limit/test_sliding_window.cpp
        tests/limit/test_key_store.cpp
        tests/limit/test_key_extractor.cpp
        tests/limit/test_sharded_limiter.cpp
        tests/http/test_request_parser.cpp
        tests/http/test_query.cpp
        tests/core/test_config.cpp
        tests/core/test_stats.cpp
        tests/net/test_listener.cpp
        tests/net/test_event_loop.cpp
    )
    target_link_libraries(rl_tests PRIVATE rl_core rl_warnings Catch2::Catch2WithMain)

    # GCC 13's TSan runtime aborts ("unexpected memory mapping") on kernels with
    # vm.mmap_rnd_bits = 32. `setarch -R` disables ASLR for just this process,
    # no root needed. catch_discover_tests runs the binary through this
    # emulator both when listing tests and when ctest runs them.
    if(RL_TSAN)
        set_property(TARGET rl_tests PROPERTY CROSSCOMPILING_EMULATOR setarch;-R)
    endif()

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(rl_tests)
endif()
