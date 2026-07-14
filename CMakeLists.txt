cmake_minimum_required(VERSION 3.20)
project(platformer CXX)

# ponytail: MSVC + CMake 4.3 can't map CXX26 yet; fall back to 23 on Windows.
# plan.md anticipated this — bump back to 26 when MSVC ships a /std:c++26 flag.
if(MSVC)
  set(CMAKE_CXX_STANDARD 23)
else()
  set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(raylib REQUIRED)   # system raylib 6.0 is installed; no FetchContent needed

# ENet (reliable-UDP) for LAN multiplayer — fetched, not a system package.
include(FetchContent)
FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG        v1.3.18)
FetchContent_MakeAvailable(enet)
# lsalzman/enet uses old-style include_directories, so its target doesn't export the path.
# ponytail: vcpkg's raylib target doesn't propagate glfw3 in all triplets, and ENet's
# CMakeLists links ws2_32/winmm PRIVATE (not INTERFACE) — so they never reach the consumer.
# Link them explicitly on Windows.
if(WIN32)
  find_package(glfw3 CONFIG REQUIRED)
endif()

# ponytail: GLOB so parallel subsystem work just drops .cpp files under src/**/ without
# editing this file (avoids merge races between agents). CONFIGURE_DEPENDS re-globs on build.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(platformer ${SOURCES})
target_include_directories(platformer PRIVATE src ${enet_SOURCE_DIR}/include)
target_link_libraries(platformer PRIVATE raylib enet)
if(WIN32)
  target_link_libraries(platformer PRIVATE glfw ws2_32 winmm)
endif()
