# Writes the ABI token and the flags of a game code DLL or dylib, without a CMake project (sdk/host/build.sh uses it for the macOS
# build):
#   cmake -DPLATFORM=macos -DSDK_DIR=<sdk> -DSOURCE_DIR=<project> -DGAME_DIR=<image folder under src/> -DOUT_DIR=<folder> -P xbe_abi.cmake
# writes <OUT_DIR>/abi_inputs.txt and <OUT_DIR>/game_code_flags.h, the files xbe_game_code_dll() writes for a Windows build, and
# prints "token <abi...>". PLATFORM=windows gives the token of the Windows build for the same tree (tools/check_game_code_dylib.sh
# compares the two: one implementation, XbeAbi.cmake).
foreach(v PLATFORM SDK_DIR SOURCE_DIR GAME_DIR OUT_DIR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "xbe_abi.cmake: -D${v}=... is needed")
    endif()
endforeach()
include(${CMAKE_CURRENT_LIST_DIR}/XbeAbi.cmake)
file(MAKE_DIRECTORY ${OUT_DIR})
xbe_abi_write(${PLATFORM} ${SDK_DIR} ${SOURCE_DIR} ${GAME_DIR} ${OUT_DIR} "")
message(STATUS "token ${abi_token}")
