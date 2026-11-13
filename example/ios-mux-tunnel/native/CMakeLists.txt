cmake_minimum_required(VERSION 3.16)
project(MuxTunnelCoreIos LANGUAGES C CXX)

# Static library with the mux-tunnelClient core and a C interface for Swift.
# Built once per slice (iphoneos arm64, iphonesimulator arm64 / x86_64) by scripts/buildNativeCore.sh.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(NETRECTOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NETRECTOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NETRECTOR_ENABLE_TLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPOSITORY_ROOT}" netRectorClient)

set(CLIENT_SOURCE_DIR "${REPOSITORY_ROOT}/example/mux-tunnelClient/src")
file(GLOB CLIENT_SOURCES CONFIGURE_DEPENDS "${CLIENT_SOURCE_DIR}/*.cpp")
list(FILTER CLIENT_SOURCES EXCLUDE REGEX "/main\\.cpp$")

add_library(muxTunnelCore STATIC
    ${CLIENT_SOURCES}
    MuxTunnelCore.cpp
    MuxTunnelUtunFinder.c)
target_include_directories(muxTunnelCore PRIVATE "${CLIENT_SOURCE_DIR}" PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(muxTunnelCore PUBLIC netRectorClient)
