cmake_minimum_required(VERSION 3.15)
project(NvidiaNimProxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Discover packages from vcpkg
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(httplib REQUIRED)

add_executable(nim-balancer main.cpp)

target_link_libraries(nim-balancer PRIVATE 
    CURL::libcurl 
    nlohmann_json::nlohmann_json
    httplib::httplib
)

# Native MSVC Library attachments
if(WIN32)
    target_link_libraries(nim-balancer PRIVATE ws2_32 crypt32 bcrypt)
endif()
