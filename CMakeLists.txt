cmake_minimum_required(VERSION 3.16)
project(NvidiaNimProxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# On Windows, require Windows 10+ for cpp-httplib. Trick it here
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

# Threads are strictly required on Linux/macOS for std::thread support
find_package(Threads REQUIRED)

# Build curl from submodule (configured via cmake/curl.cmake)
include(cmake/curl.cmake)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NIM_BUILD_TESTS "Build the unit test suite (Catch2 from thirdparty/catch2)" ON)
option(NIM_ENABLE_COVERAGE "Build with coverage instrumentation (GCC/Clang; requires gcov/gcovr)" OFF)

# ---------------------------------------------------------------------------
# Core library: all logic except main() wiring (shared by server + tests)
# ---------------------------------------------------------------------------
add_library(nim-core STATIC utils.cpp anthropic_handler.cpp proxy_handlers.cpp)

target_include_directories(nim-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nim-core PUBLIC Threads::Threads CURL::libcurl)

# MSVC-specific: enable UTF-8 source and execution charset
if(MSVC)
    target_compile_options(nim-core PRIVATE "/utf-8")
endif()

# Native MSVC Library attachments for Windows socket operations
if(WIN32)
    target_link_libraries(nim-core PUBLIC ws2_32 crypt32 bcrypt)
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(nim-balancer main.cpp)
target_link_libraries(nim-balancer PRIVATE nim-core)

# ---------------------------------------------------------------------------
# Coverage helper: add --coverage instrumentation for GCC/Clang builds
# ---------------------------------------------------------------------------
function(nim_enable_coverage TGT)
    if(NIM_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${TGT} PRIVATE --coverage -O0 -g)
        # PUBLIC so consumers linking the static lib pull in the gcov runtime
        target_link_options(${TGT} PUBLIC --coverage)
    endif()
endfunction()

nim_enable_coverage(nim-core)

# ---------------------------------------------------------------------------
# Unit tests (Catch2 v3 from thirdparty/catch2 submodule)
# ---------------------------------------------------------------------------
if(NIM_BUILD_TESTS)
    include(CTest)
    add_subdirectory(thirdparty/catch2)

    add_executable(nim-tests
        tests/test_logger.cpp
        tests/test_utils.cpp
        tests/test_key_manager.cpp
        tests/test_proxy_config.cpp
        tests/test_stats_collector.cpp
        tests/test_anthropic_handler.cpp
        tests/test_provider_manager.cpp
        tests/test_handlers.cpp
        tests/test_handlers_b.cpp
        tests/test_handlers_extra.cpp
    )
    target_link_libraries(nim-tests PRIVATE nim-core Catch2::Catch2WithMain)

    if(MSVC)
        target_compile_options(nim-tests PRIVATE "/utf-8")
    endif()

    nim_enable_coverage(nim-tests)

    add_test(NAME nim-tests COMMAND nim-tests)
endif()
