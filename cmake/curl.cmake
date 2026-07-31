# Configure curl build options before adding as subdirectory
# This is included from the main CMakeLists.txt

# Build only static library, no executables, no tests, no docs
set(BUILD_CURL_EXE OFF)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_STATIC_LIBS ON)
set(CURL_STATICLIB ON)
set(CURL_DISABLE_INSTALL ON)
set(HTTP_ONLY ON)
set(CURL_BUILD_DOCS OFF)
set(CURL_BUILD_TESTING OFF)
set(CURL_BUILD_EXAMPLES OFF)
set(CURL_USE_LIBPSL OFF)
set(CURL_USE_LIBSSH2 OFF)
set(CURL_USE_LIBRTMP OFF)
set(CURL_USE_LIBIDN2 OFF)
set(CURL_USE_LIBGSASL OFF)
set(CURL_USE_BROTLI OFF)
set(CURL_USE_ZSTD OFF)
set(ENABLE_CURL_MANUAL OFF)

# Platform-specific TLS backend
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
else()
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
    # On Linux/macOS, find OpenSSL
    find_package(OpenSSL REQUIRED)
endif()

# Add curl as subdirectory from thirdparty
add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/curl ${CMAKE_CURRENT_BINARY_DIR}/curl)
