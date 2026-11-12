cmake_minimum_required(VERSION 3.16)

project(MuxTunnelClient LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MUXTUNNEL_STATIC "Link the executable fully static (Linux only)" OFF)

# Standalone build: add the library from the repository root. As a subdirectory of the
# netRectorClient project the target already exists.
if(NOT TARGET netRectorClient)
    set(NETRECTOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NETRECTOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_BINARY_DIR}/netRectorClient")
endif()

add_executable(muxTunnelClient
    src/main.cpp
    src/ClientApplication.cpp
    src/ClientCommandLine.cpp
    src/ClientTunnel.cpp
    src/LocalSocksConnection.cpp
    src/TunnelPool.cpp
    src/WebSocketClientHandshake.cpp
    src/Socks5GreetingParser.cpp
    src/Socks5Reply.cpp
    src/findHeaderValue.cpp
    src/parseUnsigned.cpp
    src/splitHostPort.cpp
    src/raiseFileDescriptorLimit.cpp
    src/TerminationSignal.cpp
)

target_include_directories(muxTunnelClient PRIVATE src)
target_link_libraries(muxTunnelClient PRIVATE netRectorClient)
if(MSVC)
    target_compile_options(muxTunnelClient PRIVATE /W3)
else()
    target_compile_options(muxTunnelClient PRIVATE -Wall -Wextra)
endif()

set_target_properties(muxTunnelClient PROPERTIES OUTPUT_NAME "mux-tunnelClient")

if(MUXTUNNEL_STATIC AND UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_options(muxTunnelClient PRIVATE -static -static-libgcc -static-libstdc++)
endif()
