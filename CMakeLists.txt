cmake_minimum_required(VERSION 3.15)
project(NvidiaNimProxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Threads are strictly required on Linux/macOS for std::thread support
find_package(Threads REQUIRED)

# 2. Curl is required on all platforms
find_package(CURL REQUIRED)

# 3. Handle nlohmann_json (Either via system config or fallback to manual search/include)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via CMake config. Searching local/system include paths...")
    # Looks for 'nlohmann/json.hpp' in system include dirs or local workspace
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp PATHS ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# 4. Handle httplib (Either via system config or fallback to local directory)
find_package(httplib QUIET)
if(NOT httplib_FOUND)
    message(STATUS "httplib not found via CMake config. Assuming local fallback...")
    # Looks for 'httplib.h' in the current project directory or system path
    find_path(HTTPLIB_INCLUDE_DIR httplib.h PATHS ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Define Executable
add_executable(nim-balancer main.cpp)

# Link core dependencies (Pthread is automatically attached to Linux targets here)
target_link_libraries(nim-balancer PRIVATE Threads::Threads CURL::libcurl)

# Attach nlohmann_json (Targets vs header directory fallbacks)
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(nim-balancer PRIVATE nlohmann_json::nlohmann_json)
elseif(NLOHMANN_JSON_INCLUDE_DIR)
    target_include_directories(nim-balancer PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

# Attach httplib (Targets vs local directory fallbacks)
if(TARGET httplib::httplib)
    target_link_libraries(nim-balancer PRIVATE httplib::httplib)
elseif(HTTPLIB_INCLUDE_DIR)
    target_include_directories(nim-balancer PRIVATE ${HTTPLIB_INCLUDE_DIR})
endif()

# Native MSVC Library attachments for Windows socket operations
if(WIN32)
    target_link_libraries(nim-balancer PRIVATE ws2_32 crypt32 bcrypt)
endif()
