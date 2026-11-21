# The recompiler, the XDK signature scanner and this port's config tool on their own: for this machine (regen.sh), or cross built
# for Windows with llvm-mingw, which is what the first start of the Windows build runs on the player's PC (CMakePresets.json here:
# `cmake --preset win-x64-mingw` in this folder). The SDK's XbeRecomp.cmake also builds the Windows host, so these are declared
# here until the macOS build replaces it.
cmake_minimum_required(VERSION 3.22)
project(xbe_tools CXX C)
# Windows: every program carries a manifest that makes its command line and narrow file names UTF-8 (sdk/tools/tool.manifest)
if(WIN32)
    enable_language(RC)
    file(WRITE ${CMAKE_BINARY_DIR}/tool_manifest.rc "1 24 \"${CMAKE_CURRENT_LIST_DIR}/../sdk/tools/tool.manifest\"\n")
    set(TOOL_RC ${CMAKE_BINARY_DIR}/tool_manifest.rc)
    set_property(SOURCE ${TOOL_RC} APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../sdk/tools/tool.manifest)
endif()
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
get_filename_component(SDK ${CMAKE_CURRENT_LIST_DIR}/../sdk ABSOLUTE)

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis tomlplusplus)

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS ${SDK}/tools/recomp/*.cpp)
add_executable(xbe_recomp ${RECOMP_SOURCES} ${TOOL_RC})
target_include_directories(xbe_recomp PRIVATE ${SDK}/runtime ${SDK}/tools/recomp)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

add_executable(xbe_symbols ${SDK}/tools/symbols/main.cpp ${TOOL_RC})
target_include_directories(xbe_symbols PRIVATE ${SDK}/runtime)
target_link_libraries(xbe_symbols PRIVATE Zydis tomlplusplus::tomlplusplus)

# Hellmouth's config tool: the port of make_config.py that writes config/game.toml from final.map, and the list of the SDK's names
# it binds, which it finds in the runtime's sources at build time (sdk_names.txt, beside the program: the player's PC has no sources)
# (with pictures.cpp: the achievement pictures and the icon the first start makes from the disc, `--pictures`; stb_image_write writes the PNGs)
add_executable(buffy_config ${CMAKE_CURRENT_LIST_DIR}/buffy_config/main.cpp ${CMAKE_CURRENT_LIST_DIR}/buffy_config/pictures.cpp ${TOOL_RC})
target_include_directories(buffy_config PRIVATE ${SDK}/runtime)
if(CMAKE_CROSSCOMPILING)
    # the names are found by a program for this machine
    set(XBE_HOST_TOOLS "" CACHE PATH "Folder with buffy_config built for this machine (a native build of this folder), when cross-compiling")
    if(NOT XBE_HOST_TOOLS OR NOT EXISTS ${XBE_HOST_TOOLS}/buffy_config)
        message(FATAL_ERROR "cross-compiling: set XBE_HOST_TOOLS to the folder holding a buffy_config built for this machine "
                            "(tools/regen.sh builds it in build/tools, or: cmake --build build/tools --target buffy_config)")
    endif()
    set(names_tool ${XBE_HOST_TOOLS}/buffy_config)
    set(names_tool_dep ${XBE_HOST_TOOLS}/buffy_config)
else()
    set(names_tool buffy_config)
    set(names_tool_dep buffy_config)
endif()
file(GLOB_RECURSE SDK_NAME_SOURCES CONFIGURE_DEPENDS ${SDK}/runtime/*.cpp ${SDK}/runtime/*.h ${SDK}/xdk/*/signatures.toml)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/sdk_names.txt
    COMMAND ${names_tool} --scan-sdk ${SDK} --out ${CMAKE_BINARY_DIR}/sdk_names.txt
    DEPENDS ${names_tool_dep} ${SDK_NAME_SOURCES}
    COMMENT "Listing the SDK's names for buffy_config"
    VERBATIM)
add_custom_target(buffy_config_names ALL DEPENDS ${CMAKE_BINARY_DIR}/sdk_names.txt)
