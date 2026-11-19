cmake_minimum_required(VERSION 3.16)
project(netRectorClient VERSION 1.0.0 LANGUAGES CXX)

# netRectorClient: static, minimal, cross-platform (Linux, Android, iOS/macOS, Windows) variant of
# netRector. poll()/select() event loop instead of epoll shards, system DNS, no timers.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NETRECTOR_BUILD_EXAMPLES "Build the examples" ON)
option(NETRECTOR_BUILD_TESTS "Build the tests" ON)
option(NETRECTOR_ENABLE_TLS "Enable TLS (needs BoringSSL)" ON)
set(BORINGSSL_INCLUDE_DIR "" CACHE PATH "BoringSSL headers directory (contains openssl/ssl.h), for a non-standard layout")
set(BORINGSSL_LIB_DIR "" CACHE PATH "BoringSSL libraries directory (contains libssl.a, libcrypto.a), for a non-standard layout")
set(BORINGSSL_ROOT "" CACHE PATH "BoringSSL install or build directory (contains include/ and libssl.a, libcrypto.a)")

file(GLOB NETRECTOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB NETRECTOR_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

# The library is always static
add_library(netRectorClient STATIC ${NETRECTOR_SOURCES} ${NETRECTOR_HEADERS})
add_library(netRectorClient::netRectorClient ALIAS netRectorClient)
target_include_directories(netRectorClient PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include/netRectorClient>)

if(MSVC)
    target_compile_options(netRectorClient PRIVATE /W3 /utf-8)
    target_compile_definitions(netRectorClient PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(netRectorClient PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---- platform libraries ----
if(WIN32)
    target_link_libraries(netRectorClient PUBLIC ws2_32 mswsock)
elseif(ANDROID)
    target_link_libraries(netRectorClient PUBLIC log)
endif()
find_package(Threads REQUIRED)
target_link_libraries(netRectorClient PUBLIC Threads::Threads)

# ---- TLS (BoringSSL) ----
set(NETRECTOR_TLS_FOUND OFF)
if(NETRECTOR_ENABLE_TLS)
    # Non-standard layout: -DBORINGSSL_INCLUDE_DIR=<dir with openssl/ssl.h> -DBORINGSSL_LIB_DIR=<dir with libssl.a, libcrypto.a>
    set(_bssl_hints ${BORINGSSL_ROOT} $ENV{BORINGSSL_ROOT})
    set(_bssl_inc_hints ${BORINGSSL_INCLUDE_DIR} ${_bssl_hints})
    set(_bssl_lib_hints ${BORINGSSL_LIB_DIR} ${_bssl_hints})
    find_path(BSSL_INCLUDE_DIR openssl/ssl.h HINTS ${_bssl_inc_hints} PATH_SUFFIXES include . NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(BSSL_SSL_LIB NAMES ssl libssl HINTS ${_bssl_lib_hints} PATH_SUFFIXES lib . ssl Release NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(BSSL_CRYPTO_LIB NAMES crypto libcrypto HINTS ${_bssl_lib_hints} PATH_SUFFIXES lib . crypto Release NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

    if(BSSL_INCLUDE_DIR AND BSSL_SSL_LIB AND BSSL_CRYPTO_LIB)
        # needs SSL_set_record_protocol_version / TLS_CL_VERSION: scripts/patch_boringssl.sh, applied BEFORE building BoringSSL
        file(READ "${BSSL_INCLUDE_DIR}/openssl/ssl.h" _bssl_ssl_h)
        if(NOT _bssl_ssl_h MATCHES "SSL_set_record_protocol_version" OR NOT _bssl_ssl_h MATCHES "TLS_CL_VERSION")
            message(FATAL_ERROR "netRectorClient: this BoringSSL (${BSSL_INCLUDE_DIR}) lacks SSL_set_record_protocol_version / "
                                "TLS_CL_VERSION. Run scripts/patch_boringssl.sh <boringssl-src> on the sources and rebuild BoringSSL "
                                "(scripts/build_boringssl.sh does both), or use -DNETRECTOR_ENABLE_TLS=OFF.")
        endif()
        set(NETRECTOR_TLS_FOUND ON)
        message(STATUS "netRectorClient: TLS enabled (BoringSSL: ${BSSL_SSL_LIB})")
        target_include_directories(netRectorClient PUBLIC ${BSSL_INCLUDE_DIR})
        target_link_libraries(netRectorClient PUBLIC ${BSSL_SSL_LIB} ${BSSL_CRYPTO_LIB})
        target_compile_definitions(netRectorClient PUBLIC NETRECTOR_HAS_TLS=1)

        # The customized BoringSSL has SSL_set_record_protocol_version and TLS_CL_VERSION, a stock one has
        # not. Detect it (a declaration check, so it also works when cross-compiling).
        include(CheckCXXSourceCompiles)
        set(_saved_required_includes ${CMAKE_REQUIRED_INCLUDES})
        set(_saved_try_type ${CMAKE_TRY_COMPILE_TARGET_TYPE})
        set(CMAKE_REQUIRED_INCLUDES ${BSSL_INCLUDE_DIR})
        set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
        check_cxx_source_compiles("
            #include <openssl/ssl.h>
            void f(SSL* ssl) { SSL_set_record_protocol_version(ssl, TLS_CL_VERSION); }
            int main() { return 0; }" NETRECTOR_BSSL_RECORD_VERSION)
        set(CMAKE_REQUIRED_INCLUDES ${_saved_required_includes})
        set(CMAKE_TRY_COMPILE_TARGET_TYPE ${_saved_try_type})
        if(NETRECTOR_BSSL_RECORD_VERSION)
            target_compile_definitions(netRectorClient PUBLIC NETRECTOR_BSSL_RECORD_VERSION=1)
        else()
            message(STATUS "netRectorClient: stock BoringSSL (no SSL_set_record_protocol_version), the record-version customization is skipped")
        endif()
    else()
        message(WARNING "netRectorClient: BoringSSL not found (set -DBORINGSSL_ROOT=... or -DBORINGSSL_INCLUDE_DIR=... -DBORINGSSL_LIB_DIR=...), building WITHOUT TLS")
    endif()
endif()

# ---- install ----
include(GNUInstallDirs)
install(TARGETS netRectorClient ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${NETRECTOR_HEADERS} DESTINATION include/netRectorClient)

if(NETRECTOR_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/example/mux-tunnelClient/CMakeLists.txt)
    add_subdirectory(example/mux-tunnelClient)
endif()

if(NETRECTOR_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    enable_testing()
    add_subdirectory(tests)
endif()
