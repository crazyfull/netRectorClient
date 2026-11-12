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
    set(_bssl_hints ${BORINGSSL_ROOT} $ENV{BORINGSSL_ROOT})
    find_path(BSSL_INCLUDE_DIR openssl/ssl.h HINTS ${_bssl_hints} PATH_SUFFIXES include NO_DEFAULT_PATH)
    find_library(BSSL_SSL_LIB NAMES ssl libssl HINTS ${_bssl_hints} PATH_SUFFIXES lib . ssl Release NO_DEFAULT_PATH)
    find_library(BSSL_CRYPTO_LIB NAMES crypto libcrypto HINTS ${_bssl_hints} PATH_SUFFIXES lib . crypto Release NO_DEFAULT_PATH)

    if(BSSL_INCLUDE_DIR AND BSSL_SSL_LIB AND BSSL_CRYPTO_LIB)
        set(NETRECTOR_TLS_FOUND ON)
        message(STATUS "netRectorClient: TLS enabled (BoringSSL: ${BSSL_SSL_LIB})")
        target_include_directories(netRectorClient PUBLIC ${BSSL_INCLUDE_DIR})
        target_link_libraries(netRectorClient PUBLIC ${BSSL_SSL_LIB} ${BSSL_CRYPTO_LIB})
        target_compile_definitions(netRectorClient PUBLIC NETRECTOR_HAS_TLS=1)
    else()
        message(WARNING "netRectorClient: BoringSSL not found (set -DBORINGSSL_ROOT=...), building WITHOUT TLS")
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
