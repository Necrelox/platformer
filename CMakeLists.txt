cmake_minimum_required(VERSION 3.20)
project(platformer CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(raylib REQUIRED)   # system raylib 6.0 is installed; no FetchContent needed

# ENet (reliable-UDP) for LAN multiplayer — fetched, not a system package.
include(FetchContent)
FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG        v1.3.18)
FetchContent_MakeAvailable(enet)
# lsalzman/enet uses old-style include_directories, so its target doesn't export the path.
# ponytail: on Windows the `enet` target already links ws2_32/winmm via its own CMakeLists.

# ponytail: GLOB so parallel subsystem work just drops .cpp files under src/**/ without
# editing this file (avoids merge races between agents). CONFIGURE_DEPENDS re-globs on build.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(platformer ${SOURCES})
target_include_directories(platformer PRIVATE src ${enet_SOURCE_DIR}/include)
target_link_libraries(platformer PRIVATE raylib enet)
