# Original Xbox static recompilation: xbe_recomp, the host runtime and add_xbe_image()
include_guard(GLOBAL)

get_filename_component(XBE_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
# build scripts find the SDK's xdk folder and THIRD_PARTY.txt through this
file(TO_NATIVE_PATH "${XBE_SDK_DIR}" XBE_SDK_DIR_NATIVE)
file(WRITE ${CMAKE_BINARY_DIR}/sdk_dir.txt "${XBE_SDK_DIR_NATIVE}\n")

# the C and C++ runtimes built into every program (/MT): the exe runs without the Visual C++ Redistributable. Set
# before the libraries below so they all match, and for those that ask an older CMake as well (CMP0091)
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

include(FetchContent)
# the executables' manifest (runtime/app/app.manifest) goes in as a resource
if(WIN32)
    enable_language(RC)
    # the linker must not add its own manifest (also resource 1 of type 24): clang's link line has /MANIFEST:EMBED
    # built in, which lld-link turns into "duplicate resource" against ours
    foreach(lang C CXX)
        string(REPLACE "/MANIFEST:EMBED" "/MANIFEST:NO" CMAKE_${lang}_LINK_EXECUTABLE "${CMAKE_${lang}_LINK_EXECUTABLE}")
    endforeach()
    if(MSVC AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        add_link_options(/MANIFEST:NO)
    endif()
endif()
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
# SDL3 for controllers, static so it links into the exe
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.18 GIT_SHALLOW TRUE)
# glslang, static, for the Vulkan backend's HLSL to SPIR-V at run time (runtime/gpu/spirv.cpp): its HLSL front end
# alone, no optimizer (SPIRV-Tools), no tools or tests
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
foreach(opt ENABLE_OPT ENABLE_GLSLANG_BINARIES ENABLE_SPVREMAPPER ENABLE_CTEST GLSLANG_TESTS BUILD_EXTERNAL)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 15.4.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis tomlplusplus imgui SDL3 glslang)

# Cross-compiling (an ARM64 build on an x64 PC): the tools must run on this machine, so they come from a build
# for it, XBE_HOST_TOOLS = the folder holding its xbe_recomp and xbe_symbols
set(XBE_PGO "" CACHE STRING "Profile-guided optimisation: GEN (a measuring build), OFF, or empty (use pgo/<target>.profdata when there)")
set(XBE_HOST_TOOLS "" CACHE PATH "Folder with xbe_recomp and xbe_symbols built for this machine, when cross-compiling")
# the generated code is already in generated/<dir> (made by tools/regen.sh on another machine): no recompile step, and
# the tools aren't built, which a cross build to Windows from another OS cannot run
option(XBE_USE_GENERATED "Build from the existing generated/<dir> folder instead of running xbe_recomp" OFF)
# the recompiled game code goes into <target>.dll, beside the executable, and not into it: the executable then holds none of
# it, and the DLL can be compiled where the game is installed (llvm-mingw, from the xbe that is there). The DLL is
# built here only as a check, by the game_code_dll target (see add_xbe_image).
option(XBE_GAME_CODE_DLL "Build the recompiled game code into <target>.dll instead of the executable" OFF)
if(XBE_GAME_CODE_DLL AND NOT MINGW)
    message(FATAL_ERROR "XBE_GAME_CODE_DLL needs the llvm-mingw toolchain (sdk/cmake/llvm-mingw.cmake)")
endif()
if(XBE_USE_GENERATED)
    # nothing to build
elseif(XBE_HOST_TOOLS)
    foreach(tool xbe_recomp xbe_symbols)
        add_executable(${tool} IMPORTED GLOBAL)
        set_target_properties(${tool} PROPERTIES IMPORTED_LOCATION ${XBE_HOST_TOOLS}/${tool}${CMAKE_HOST_EXECUTABLE_SUFFIX})
    endforeach()
else()
    # x86 to C++ recompiler
    file(GLOB XBE_RECOMP_SOURCES CONFIGURE_DEPENDS ${XBE_SDK_DIR}/tools/recomp/*.cpp)
    add_executable(xbe_recomp ${XBE_RECOMP_SOURCES})
    target_include_directories(xbe_recomp PRIVATE ${XBE_SDK_DIR}/runtime ${XBE_SDK_DIR}/tools/recomp)
    target_compile_definitions(xbe_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

    # XDK signature scanner that writes a new title's config
    add_executable(xbe_symbols ${XBE_SDK_DIR}/tools/symbols/main.cpp)
    target_include_directories(xbe_symbols PRIVATE ${XBE_SDK_DIR}/runtime)
    target_compile_definitions(xbe_symbols PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(xbe_symbols PRIVATE Zydis tomlplusplus::tomlplusplus)
endif()

set(XBE_IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
# the Vulkan menu backend calls Vulkan through the functions gpu/vk_loader.cpp looks up (no vulkan-1.lib)
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

# the in-game menu reads the controllers through SDL (app/overlay.cpp), not the backend's XInput
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
file(GLOB XBE_RUNTIME_SOURCES CONFIGURE_DEPENDS ${XBE_SDK_DIR}/runtime/*/*.cpp)
# SDL's headers go to the input code alone (it is linked LINK_ONLY), so the generated sources' compile line stays the same
set_source_files_properties(${XBE_SDK_DIR}/runtime/core/input.cpp PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")
# glslang's headers likewise go to spirv.cpp alone (the kit records only the include folders of the recompiled code)
set_source_files_properties(${XBE_SDK_DIR}/runtime/gpu/spirv.cpp PROPERTIES INCLUDE_DIRECTORIES "${glslang_SOURCE_DIR}")

# The game code DLL's side of add_xbe_image: what the executable needs to be linked to it, and a target that builds the DLL
# here, with the commands the player's PC runs at its first start (the compile and link flags below are the ones
# that go into the executable as game_code_flags.h). The DLL is compiled for the mingw target without a C or C++ library, so
# it holds only the recompiled code: it imports the runtime and its overrides from the executable, which exports them
# (X86_HOST_API and X86_FUNC in cpu/x86_ops.h, host_exports.def.in), and the executable imports its recompiled functions
# from the DLL by the names of imports.def, delay loaded.
function(xbe_game_code_dll target GEN GEN_SOURCES CONFIG_FILE GAME_DIR)
    # the DLL's name is the one the executable delay loads and imports.def names: the config's [xbe] library must be <target>.dll
    # (the executable's name is <target>.exe, and the DLL's imports name it). A config that is not there yet (build.bat writes it
    # at the first build) is not checked.
    if(EXISTS ${CONFIG_FILE})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONFIG_FILE})
        set(config_library "")
        file(STRINGS ${CONFIG_FILE} config_library_line REGEX "^library[ \t]*=")
        if(config_library_line MATCHES "^library[ \t]*=[ \t]*\"([^\"]*)\"")
            set(config_library "${CMAKE_MATCH_1}")
        endif()
        if(NOT config_library STREQUAL "${target}.dll")
            if(config_library)
                set(config_problem "has [xbe] library = \"${config_library}\"")
            else()
                set(config_problem "has no [xbe] library")
            endif()
            message(FATAL_ERROR "${CONFIG_FILE} ${config_problem}, but a build with XBE_GAME_CODE_DLL needs library = \"${target}.dll\": "
                "the DLL is named after the executable's target (${target}), as its imports name ${target}.exe and the executable "
                "delay loads ${target}.dll. Set it in [xbe] (xbe_symbols writes it from --library ${target}.dll).")
        endif()
    endif()
    set(CODE ${CMAKE_BINARY_DIR}/game_code)
    get_filename_component(llvm_bin ${CMAKE_CXX_COMPILER} DIRECTORY)
    find_program(XBE_LLVM_LIB llvm-lib HINTS ${llvm_bin} REQUIRED NO_DEFAULT_PATH)
    find_program(XBE_CLANG clang HINTS ${llvm_bin} REQUIRED NO_DEFAULT_PATH)
    find_program(XBE_LLD_LINK lld-link HINTS ${llvm_bin} REQUIRED NO_DEFAULT_PATH)

    # The ABI stamp: ties a game code DLL to the executable it was built for. The DLL binds to the executable by names, so one made
    # for another version would load and call the wrong code. The token is a hash of what makes a DLL fit this executable: the
    # headers the code is compiled against (the runtime's ABI and the stand-ins for the C++ library), the compile and link flags,
    # the recompiler (what code it writes), the module definition file, and the sources that define the functions the executable
    # replaces and so exports (runtime/xdk and src/<game_dir>: whole folders, so that a replacement added anywhere in them is
    # counted; check_game_code_dll.sh fails if the executable exports one from anywhere else). It goes to the DLL in its compile
    # (-DXBE_GAME_CODE_ABI=<token>), which stores it in the DLL's X86GameCodeStamp beside the image and names it was made from, and into
    # the executable (game_code_flags.h, for game_code.cpp), which compares the two before it uses the DLL. A change to any of these
    # files gives the DLL a new compile: this errs towards compiling again. Written to game_code/abi_inputs.txt.
    file(GLOB freestanding CONFIGURE_DEPENDS ${XBE_SDK_DIR}/runtime/cpu/freestanding/*)
    set(headers ${XBE_SDK_DIR}/runtime/cpu/x86_ops.h ${XBE_SDK_DIR}/runtime/cpu/x86_context.h)
    set(override_dirs ${XBE_SDK_DIR}/runtime/xdk ${CMAKE_CURRENT_SOURCE_DIR}/src/${GAME_DIR})
    file(GLOB_RECURSE override_files CONFIGURE_DEPENDS ${XBE_SDK_DIR}/runtime/xdk/* ${CMAKE_CURRENT_SOURCE_DIR}/src/${GAME_DIR}/*)
    file(GLOB recomp_files CONFIGURE_DEPENDS ${XBE_SDK_DIR}/tools/recomp/*.cpp ${XBE_SDK_DIR}/tools/recomp/*.h)
    set(abi_files ${headers} ${freestanding} ${XBE_SDK_DIR}/runtime/cpu/host_exports.def.in ${override_files} ${recomp_files})
    list(REMOVE_DUPLICATES abi_files)
    list(SORT abi_files)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${abi_files})

    # the flags of the first start (and of game_code_flags.h): one place
    set(compile_flags "-target x86_64-w64-mingw32 -std=c++20 -O2 -march=x86-64-v2 -fno-strict-aliasing -fms-extensions -nostdinc -nostdinc++ -w -DNDEBUG -DX86_GAME_CODE")
    set(link_flags "/dll /noentry /nodefaultlib /machine:x64 /opt:noicf")
    set(abi_text "xbe-game-code-abi 1\ncompile ${compile_flags}\nlink ${link_flags}\n")
    foreach(f ${abi_files})
        file(SHA1 ${f} f_sha1)
        file(RELATIVE_PATH f_rel ${CMAKE_CURRENT_SOURCE_DIR} ${f})
        string(APPEND abi_text "${f_sha1} ${f_rel}\n")
    endforeach()
    string(SHA1 abi_sha1 "${abi_text}")
    string(SUBSTRING ${abi_sha1} 0 32 abi_hex)
    set(abi_token abi${abi_hex})   # a bare token, so that no quote passes through a shell or a command line
    set(abi_dirs)
    foreach(d ${override_dirs})
        file(RELATIVE_PATH d_rel ${CMAKE_CURRENT_SOURCE_DIR} ${d})
        string(APPEND abi_dirs "override-dir ${d_rel}\n")
    endforeach()
    file(WRITE ${CODE}/abi_inputs.txt "token ${abi_token}\n${abi_dirs}${abi_text}")
    # the compile gets the token; the hash above does not include it
    string(APPEND compile_flags " -DXBE_GAME_CODE_ABI=${abi_token}")
    set(game_code_flags "// written by CMake (xbe_game_code_dll): the flags that compile and link the game code DLL, and its ABI token\n#pragma once\n#define XBE_GAME_CODE_ABI ${abi_token}\n#define XBE_GAME_CODE_COMPILE_FLAGS L\"${compile_flags}\"\n#define XBE_GAME_CODE_LINK_FLAGS L\"${link_flags}\"\n")
    file(CONFIGURE OUTPUT ${CODE}/game_code_flags.h CONTENT "${game_code_flags}" @ONLY)
    separate_arguments(compile_args UNIX_COMMAND "${compile_flags}")
    separate_arguments(link_args UNIX_COMMAND "${link_flags}")

    # the executable's side: the import library that names where each recompiled function is, the delay loading, and the
    # module definition file with what the DLL imports that is not the runtime's own
    add_custom_command(
        OUTPUT ${CODE}/code.lib
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CODE}
        COMMAND ${XBE_LLVM_LIB} /def:${GEN}/imports.def /out:${CODE}/code.lib /machine:x64
        DEPENDS ${GEN}/imports.def
        COMMENT "Import library of ${target}.dll"
        VERBATIM)
    add_custom_target(${target}_imports DEPENDS ${CODE}/code.lib)
    if(NOT XBE_USE_GENERATED)
        add_dependencies(${target}_imports recompile_${target})  # imports.def is a byproduct of the recompile
    endif()
    add_dependencies(${target} ${target}_imports)
    set(XBE_TARGET ${target})
    configure_file(${XBE_SDK_DIR}/runtime/cpu/host_exports.def.in ${CODE}/host_exports.def @ONLY)
    # the executable's own import library is what the DLL links against, as <target>.lib (not ENABLE_EXPORTS, which on mingw
    # exports every symbol)
    set(exe_implib ${CODE}/${target}.lib)
    set_target_properties(${target} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CODE} IMPORT_PREFIX "" IMPORT_SUFFIX ".lib")
    target_compile_definitions(${target} PRIVATE XBE_GAME_CODE_DLL)
    target_include_directories(${target} PRIVATE ${CODE})
    target_link_libraries(${target} PRIVATE ${CODE}/code.lib)
    target_link_options(${target} PRIVATE ${CODE}/host_exports.def -Wl,--delayload=${target}.dll)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${CODE}/code.lib ${CODE}/host_exports.def)

    # the headers the generated code includes, where the first start finds them: <include>/cpu, <include>/freestanding
    add_custom_command(
        OUTPUT ${CODE}/include.stamp
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CODE}/include/cpu ${CODE}/include/freestanding
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${headers} ${CODE}/include/cpu
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${freestanding} ${CODE}/include/freestanding
        COMMAND ${CMAKE_COMMAND} -E touch ${CODE}/include.stamp
        DEPENDS ${headers} ${freestanding}
        VERBATIM)

    # check only: the DLL built here, a compile for each generated file and then a link, as the first start does them
    # (in the folder of the sources, the objects beside the DLL)
    set(objects)
    foreach(src ${GEN_SOURCES})
        get_filename_component(name ${src} NAME_WLE)
        set(object ${CODE}/${name}.obj)
        add_custom_command(
            OUTPUT ${object}
            COMMAND ${XBE_CLANG} ${compile_args} -isystem ${CODE}/include/freestanding -I${CODE}/include
                    -c ${name}.cpp -o ${object}
            DEPENDS ${src} ${CODE}/include.stamp ${GEN}/x86_recomp_shared.h
            WORKING_DIRECTORY ${GEN}
            COMMENT "Compiling ${name}.cpp for ${target}.dll"
            VERBATIM)
        list(APPEND objects ${object})
    endforeach()
    add_custom_command(
        OUTPUT ${CODE}/${target}.dll
        COMMAND ${XBE_LLD_LINK} ${link_args} /def:${GEN}/exports.def /implib:${CODE}/dll.lib /out:${CODE}/${target}.dll
                ${exe_implib} ${objects}
        DEPENDS ${objects} ${GEN}/exports.def ${target}
        COMMENT "Linking ${target}.dll"
        VERBATIM)
    add_custom_target(game_code_dll DEPENDS ${CODE}/${target}.dll)
endfunction()

# One executable per recompiled image: its code from config/<config> (partitions as that config sets them),
# the SDK runtime, XBE_HOST_SOURCES shared by every image of the game, and src/<game_dir> hooks only this image has.
# Expects assets/<xbe> and the config's out = "generated/<game_dir>". At run time the game may be a disc image instead.
function(add_xbe_image target xbe config partitions game_dir)
    set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/generated/${game_dir})
    set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
    math(EXPR last "${partitions} - 1")
    foreach(i RANGE ${last})
        list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
    endforeach()
    file(GLOB configs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/config/*.toml ${XBE_SDK_DIR}/xdk/*/*.toml)
    if(NOT XBE_USE_GENERATED)
        add_custom_command(
            OUTPUT ${GEN}/codegen.stamp
            BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h ${GEN}/exports.def ${GEN}/imports.def
            COMMAND xbe_recomp config/${config} --xdk ${XBE_SDK_DIR}/xdk
            COMMAND ${CMAKE_COMMAND} -E touch ${GEN}/codegen.stamp
            DEPENDS xbe_recomp ${configs} ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Recompiling ${xbe}"
            VERBATIM)
        add_custom_target(recompile_${target} DEPENDS ${GEN}/codegen.stamp)
    endif()
    set_source_files_properties(${GEN_SOURCES} PROPERTIES GENERATED TRUE
        COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-Wno-unused-label;-Wno-parentheses-equality;-Wno-unused-variable")
    file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${game_dir}/*.cpp)
    # the xbe's SHA-1: the executable checks the game it finds is the release it was made from
    set(xbe_file ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${xbe_file})
    file(SHA1 ${xbe_file} xbe_sha1)
    set(XBE_HASH ${CMAKE_CURRENT_BINARY_DIR}/${target}_xbe.cpp)
    file(CONFIGURE OUTPUT ${XBE_HASH} CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
    # UTF-8 paths, DPI awareness: runtime/app/app.manifest, as resource 1 of type 24 (RT_MANIFEST)
    set(XBE_RC ${CMAKE_CURRENT_BINARY_DIR}/${target}_manifest.rc)
    set(rc "1 24 \"${XBE_SDK_DIR}/runtime/app/app.manifest\"\n")
    # XBE_ICON (an .ico, set by the game before add_xbe_image): the executable's own icon, resource 1 of the icon type, which Explorer
    # shows and which the windows take by default (LoadIconW(MAKEINTRESOURCE(1))). Without it the executable has none.
    if(XBE_ICON)
        string(APPEND rc "1 ICON \"${XBE_ICON}\"\n")
        set_property(SOURCE ${XBE_RC} APPEND PROPERTY OBJECT_DEPENDS ${XBE_ICON})
    endif()
    # XBE_VERSION ("1.2.3") and XBE_PRODUCT, when the game sets them: the version Windows shows in the exe's
    # Properties, and GAME_VERSION for the code
    if(XBE_VERSION)
        string(REPLACE "." "," v "${XBE_VERSION}")
        string(APPEND rc "1 VERSIONINFO\nFILEVERSION ${v},0\nPRODUCTVERSION ${v},0\nFILEOS 0x40004\nFILETYPE 0x1\nBEGIN\n"
            "BLOCK \"StringFileInfo\"\nBEGIN\nBLOCK \"040904b0\"\nBEGIN\n"
            "VALUE \"FileDescription\", \"${XBE_PRODUCT}\"\nVALUE \"ProductName\", \"${XBE_PRODUCT}\"\n"
            "VALUE \"FileVersion\", \"${XBE_VERSION}\"\nVALUE \"ProductVersion\", \"${XBE_VERSION}\"\n"
            "VALUE \"InternalName\", \"${target}\"\nVALUE \"OriginalFilename\", \"${target}.exe\"\nEND\nEND\n"
            "BLOCK \"VarFileInfo\"\nBEGIN\nVALUE \"Translation\", 0x409, 1200\nEND\nEND\n")
    endif()
    file(CONFIGURE OUTPUT ${XBE_RC} CONTENT "${rc}")
    set_property(SOURCE ${XBE_RC} APPEND PROPERTY OBJECT_DEPENDS ${XBE_SDK_DIR}/runtime/app/app.manifest)
    if(XBE_GAME_CODE_DLL)
        add_executable(${target} WIN32 ${XBE_RUNTIME_SOURCES} ${XBE_HOST_SOURCES} ${GAME_SOURCES} ${XBE_IMGUI_SOURCES} ${XBE_HASH} ${XBE_RC})
    else()
        add_executable(${target} WIN32 ${XBE_RUNTIME_SOURCES} ${XBE_HOST_SOURCES} ${GAME_SOURCES} ${GEN_SOURCES} ${XBE_IMGUI_SOURCES} ${XBE_HASH} ${XBE_RC})
    endif()
    if(NOT XBE_USE_GENERATED)
        add_dependencies(${target} recompile_${target})
    endif()
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${XBE_SDK_DIR}/runtime ${GEN})
    target_include_directories(${target} SYSTEM PRIVATE ${imgui_SOURCE_DIR} ${XBE_SDK_DIR}/third_party/vulkan/include)
    target_compile_options(${target} PRIVATE -fno-strict-aliasing)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    if(XBE_VERSION)
        target_compile_definitions(${target} PRIVATE GAME_VERSION="${XBE_VERSION}")
    endif()
    # (mingw-w64, the llvm-mingw toolchain Athelas ships: XAudio 2.9's import library is xaudio2_9; #pragma comment(lib)
# isn't read, so gdi32 and advapi32 are named, and uuid for the file dialog's class ids)
if(MINGW)
    # mingw-w64 hides the Windows 10 calls (GetDpiForWindow, SetProcessDpiAwarenessContext) below this
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_link_options(${target} PRIVATE -municode)   # it starts at wWinMain
    set(xaudio xaudio2_9)
    set(extra gdi32 advapi32 uuid)
else()
    set(xaudio xaudio2)
    set(extra)
endif()
target_link_libraries(${target} PRIVATE tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static>
        $<LINK_ONLY:glslang::glslang> $<LINK_ONLY:glslang::SPIRV> $<LINK_ONLY:glslang::glslang-default-resource-limits> d3d12 dxgi dxguid d3dcompiler ${xaudio} ole32 windowscodecs dbghelp winmm dwmapi ws2_32 ${extra})
    # lld keeps the recompiled weak functions overridable by the host ones
    target_link_options(${target} PRIVATE -fuse-ld=lld -g)
    # profile-guided optimisation (clang): XBE_PGO=GEN builds a measuring program (it writes <target>-<pid>.profraw,
    # LLVM_PROFILE_FILE to choose); with pgo/<target>.profdata beside the port (llvm-profdata merge of those) and
    # XBE_PGO not OFF/GEN, the build uses it. The kit carries the profile, so a player's build uses it too.
    set(pgo_profile ${CMAKE_CURRENT_SOURCE_DIR}/pgo/${target}.profdata)
    if(XBE_PGO STREQUAL "GEN")
        target_compile_options(${target} PRIVATE -fprofile-instr-generate)
        target_link_options(${target} PRIVATE -fprofile-instr-generate)
        target_compile_definitions(${target} PRIVATE XBE_PGO_GEN=1)
        message(STATUS "${target}: PGO measuring build")
    elseif(NOT XBE_PGO STREQUAL "OFF" AND EXISTS ${pgo_profile} AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE -fprofile-instr-use=${pgo_profile} -Wno-profile-instr-unprofiled
                               -Wno-profile-instr-out-of-date -Wno-profile-instr-missing -Wno-backend-plugin)
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${pgo_profile})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${pgo_profile})
        message(STATUS "${target}: PGO with ${pgo_profile}")
    endif()
    if(XBE_GAME_CODE_DLL)
        xbe_game_code_dll(${target} ${GEN} "${GEN_SOURCES}" ${CMAKE_CURRENT_SOURCE_DIR}/config/${config} ${game_dir})
    endif()
endfunction()
