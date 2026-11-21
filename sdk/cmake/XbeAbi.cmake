# The ABI of a game code DLL (or dylib): the token that ties it to the executable it was built for, and the flags that build it.
# One implementation for both ways in: XbeRecomp.cmake's xbe_game_code_dll() (the Windows build) calls xbe_abi_write(), and
# xbe_abi.cmake runs it as a script (cmake -P), which is how sdk/host/build.sh gets the macOS executable's token without CMake
# building anything. See xbe_game_code_dll() for what the token is a hash of and why.
#
# The functions use no target, no property of the directory and no CMake-only command, so they run in script mode too; what a
# project does with the lists they return (re-running CMake when a file changes) is the caller's.

# The files the token hashes, as lists in the caller's scope: headers, freestanding, override_dirs, abi_files (sorted).
#   sdk_dir      the SDK (runtime/, tools/recomp/)
#   source_dir   the project's folder (src/<game_dir> is relative to it, and the paths in the token are)
#   game_dir     the image's folder under src/
#   depends      CONFIGURE_DEPENDS for a project build (the globs are looked at again when a file is added), else empty
function(xbe_abi_inputs sdk_dir source_dir game_dir depends)
    file(GLOB freestanding ${depends} ${sdk_dir}/runtime/cpu/freestanding/*)
    set(headers ${sdk_dir}/runtime/cpu/x86_ops.h ${sdk_dir}/runtime/cpu/x86_context.h)
    set(override_dirs ${sdk_dir}/runtime/xdk ${source_dir}/src/${game_dir})
    file(GLOB_RECURSE override_files ${depends} ${sdk_dir}/runtime/xdk/* ${source_dir}/src/${game_dir}/*)
    file(GLOB recomp_files ${depends} ${sdk_dir}/tools/recomp/*.cpp ${sdk_dir}/tools/recomp/*.h)
    set(abi_files ${headers} ${freestanding} ${sdk_dir}/runtime/cpu/host_exports.def.in ${override_files} ${recomp_files})
    list(REMOVE_DUPLICATES abi_files)
    list(SORT abi_files)
    set(headers ${headers} PARENT_SCOPE)
    set(freestanding ${freestanding} PARENT_SCOPE)
    set(override_dirs ${override_dirs} PARENT_SCOPE)
    set(abi_files ${abi_files} PARENT_SCOPE)
endfunction()

# Computes the token and writes <code_dir>/abi_inputs.txt and <code_dir>/game_code_flags.h (the files are not touched when they
# would not change). platform: windows (llvm-mingw, a PE DLL) or macos (arm64, a Mach-O dylib for macOS 13). In the caller's scope:
#   abi_token                  abi<32 hex>
#   compile_flags, link_flags  the flags of the first start (compile_flags with -DXBE_GAME_CODE_ABI=<token>)
#   headers, freestanding, override_dirs, abi_files   as xbe_abi_inputs
function(xbe_abi_write platform sdk_dir source_dir game_dir code_dir depends)
    xbe_abi_inputs(${sdk_dir} ${source_dir} ${game_dir} "${depends}")

    # the flags of the first start (and of game_code_flags.h): one place
    if(platform STREQUAL "windows")
        set(compile_flags "-target x86_64-w64-mingw32 -std=c++20 -O2 -march=x86-64-v2 -fno-strict-aliasing -fms-extensions -nostdinc -nostdinc++ -w -DNDEBUG -DX86_GAME_CODE")
        set(link_flags "/dll /noentry /nodefaultlib /machine:x64 /opt:noicf")
        set(flags_extra "")
    elseif(platform STREQUAL "macos")
        # no SDK: the code is compiled against the freestanding headers alone and its undefined names (the runtime, libSystem and the
        # C++ runtime) bind to the loading program when the dylib is loaded; -install_name, the response file and the output go
        # on the link line when it is run (app/game_build.cpp)
        set(compile_flags "-target arm64-apple-macos13 -std=c++20 -O2 -fno-strict-aliasing -nostdinc -nostdinc++ -w -DNDEBUG -DX86_GAME_CODE")
        set(link_flags "-dylib -arch arm64 -platform_version macos 13.0 13.0 -undefined dynamic_lookup")
        set(flags_extra "#define XBE_GAME_CODE_MACHO 1\n")
    else()
        message(FATAL_ERROR "xbe_abi_write: platform is windows or macos, not '${platform}'")
    endif()
    set(abi_text "xbe-game-code-abi 1\ncompile ${compile_flags}\nlink ${link_flags}\n")
    foreach(f ${abi_files})
        file(SHA1 ${f} f_sha1)
        file(RELATIVE_PATH f_rel ${source_dir} ${f})
        string(APPEND abi_text "${f_sha1} ${f_rel}\n")
    endforeach()
    string(SHA1 abi_sha1 "${abi_text}")
    string(SUBSTRING ${abi_sha1} 0 32 abi_hex)
    set(abi_token abi${abi_hex})   # a bare token, so that no quote passes through a shell or a command line
    set(abi_dirs)
    foreach(d ${override_dirs})
        file(RELATIVE_PATH d_rel ${source_dir} ${d})
        string(APPEND abi_dirs "override-dir ${d_rel}\n")
    endforeach()
    file(WRITE ${code_dir}/abi_inputs.txt.new "token ${abi_token}\n${abi_dirs}${abi_text}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${code_dir}/abi_inputs.txt.new ${code_dir}/abi_inputs.txt)
    file(REMOVE ${code_dir}/abi_inputs.txt.new)
    # the compile gets the token; the hash above does not include it
    string(APPEND compile_flags " -DXBE_GAME_CODE_ABI=${abi_token}")
    set(game_code_flags "// written by CMake (xbe_game_code_dll): the flags that compile and link the game code DLL, and its ABI token\n#pragma once\n#define XBE_GAME_CODE_ABI ${abi_token}\n${flags_extra}#define XBE_GAME_CODE_COMPILE_FLAGS L\"${compile_flags}\"\n#define XBE_GAME_CODE_LINK_FLAGS L\"${link_flags}\"\n")
    file(WRITE ${code_dir}/game_code_flags.h.new "${game_code_flags}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${code_dir}/game_code_flags.h.new ${code_dir}/game_code_flags.h)
    file(REMOVE ${code_dir}/game_code_flags.h.new)
    set(abi_token ${abi_token} PARENT_SCOPE)
    set(compile_flags ${compile_flags} PARENT_SCOPE)
    set(link_flags ${link_flags} PARENT_SCOPE)
    set(headers ${headers} PARENT_SCOPE)
    set(freestanding ${freestanding} PARENT_SCOPE)
    set(override_dirs ${override_dirs} PARENT_SCOPE)
    set(abi_files ${abi_files} PARENT_SCOPE)
endfunction()
