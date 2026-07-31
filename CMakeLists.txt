cmake_minimum_required(VERSION 3.15)
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

# Define Executable
add_executable(nim-balancer main.cpp utils.cpp anthropic_handler.cpp)

# Add includes and link libraries
target_include_directories(nim-balancer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(nim-balancer PRIVATE $<TARGET_PROPERTY:CURL::libcurl_static,INCLUDE_DIRECTORIES)
target_link_libraries(nim-balancer PRIVATE Threads::Threads CURL::libcurl)

# Native MSVC Library attachments for Windows socket operations
if(WIN32)
    target_link_libraries(nim-balancer PRIVATE ws2_32 crypt32 bcrypt)
endif()
