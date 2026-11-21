# Hellmouth: Buffy the Vampire Slayer (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
# the cross build's module scan fails harmlessly on SDL's files ("unsupported argument 'x86-64-v2' to option '-march='")
# and nothing here uses modules
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
project(Buffy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the Windows build. macOS builds with sdk/host/build.sh, not CMake (README.md)
set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as the config says (128)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 128)
endif()

# target name = the program's name: the game restarts itself by it (Buffy.exe), xbe = assets/Default.xbe
add_xbe_image(Buffy Default.xbe game.toml ${GAME_PARTITIONS} game)

# the settings the game has only ever run with (the macOS build's): the renderer on the game's thread, and the
# vertex data decoded rather than read raw
target_compile_definitions(Buffy PRIVATE XBE_RENDER_THREAD=0 XBE_RAW_VERTICES=0)
