cmake_minimum_required(VERSION 3.24)
project(B2SVR CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(minhook CONFIG REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

add_library(b2svr SHARED
    src/dllmain.cpp
    src/log.cpp
    src/crashreport.cpp
    src/hotkeys.cpp
    src/game.cpp
    src/probe.cpp
    src/devctl.cpp
    src/stereo.cpp
    src/xr.cpp
    src/vrinput.cpp
    src/pacing.cpp
    src/bones.cpp
    src/camdirector.cpp
    src/fpcam.cpp
    src/fpbody.cpp
    src/player.cpp
    src/heapscan.cpp
    src/watchpoint.cpp
    src/sigdb.cpp
    src/prompts.cpp
    src/timerwindow.cpp
    src/videowatch.cpp
    src/zonefix.cpp
    src/count_thunks.asm
    src/trace_thunk.asm
    src/dinput8.def
)
set_target_properties(b2svr PROPERTIES OUTPUT_NAME "dinput8")
target_compile_definitions(b2svr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(b2svr PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /Zi>)
# /PDBALTPATH: the DLL names its symbols file "dinput8.pdb" instead of the build machine's full
# path (which would carry the builder's Windows user name into every released DLL).
target_link_options(b2svr PRIVATE /DEBUG /OPT:REF /PDBALTPATH:dinput8.pdb)
target_link_libraries(b2svr PRIVATE minhook::minhook OpenXR::headers OpenXR::openxr_loader
    d3d11 dxgi bcrypt d3dcompiler)

# `cmake --build build --target deploy` copies the proxy into the game directory. The default
# finds the game next to this project (dev workspace) or two levels up (the release folder's
# source/); otherwise pass -DB2S_GAME_DIR=<folder containing BeyondTwoSouls_Steam.exe>.
set(_b2s_default_game "")
foreach(_dir "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    if(NOT _b2s_default_game AND EXISTS "${_dir}/BeyondTwoSouls_Steam.exe")
        get_filename_component(_b2s_default_game "${_dir}" ABSOLUTE)
    endif()
endforeach()
set(B2S_GAME_DIR "${_b2s_default_game}" CACHE PATH "Beyond: Two Souls install dir")
if(B2S_GAME_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:b2svr> "${B2S_GAME_DIR}/dinput8.dll"
        DEPENDS b2svr)
endif()

# Dev workspace only (game folder + ../B2S_VR_MOD + tools/publish.ps1; the release's source/ copy
# has no tools/): after every build the new DLL goes into the game folder AND the release folder,
# so the build being played and the build players download are always the same file.
# -DB2SVR_PUBLISH=OFF turns it off.
set(_b2s_release_dir "${CMAKE_CURRENT_SOURCE_DIR}/../B2S_VR_MOD")
if(B2S_GAME_DIR AND EXISTS "${_b2s_release_dir}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/publish.ps1")
    option(B2SVR_PUBLISH "After each build, install the DLL into the game and refresh ../B2S_VR_MOD" ON)
    if(B2SVR_PUBLISH)
        add_custom_command(TARGET b2svr POST_BUILD
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/publish.ps1"
                    -Dll "$<TARGET_FILE:b2svr>" -Config "$<CONFIG>" -GameDir "${B2S_GAME_DIR}"
            COMMENT "Publishing dinput8.dll to the game folder and B2S_VR_MOD"
            VERBATIM)
    endif()
endif()

# PORT-06 desk tests for the pose maths (src/posemath.h).
enable_testing()
add_executable(pose_tests tests/pose_tests.cpp)
target_compile_options(pose_tests PRIVATE /W4 /permissive-)
add_test(NAME pose_tests COMMAND pose_tests)

# F1 first-person desk tests (src/fpmode.h, src/bones_crc.h).
add_executable(fp_tests tests/fp_tests.cpp)
target_compile_options(fp_tests PRIVATE /W4 /permissive-)
add_test(NAME fp_tests COMMAND fp_tests)

# Frame-bound controller transport and loss/focus policy.
add_executable(xr_input_tests tests/xr_input_tests.cpp)
target_compile_options(xr_input_tests PRIVATE /W4 /permissive-)
add_test(NAME xr_input_tests COMMAND xr_input_tests)

add_executable(fp_math_tests tests/fp_math_tests.cpp)
target_compile_options(fp_math_tests PRIVATE /W4 /permissive-)
add_test(NAME fp_math_tests COMMAND fp_math_tests)

add_executable(fpbody_tests tests/fpbody_tests.cpp)
target_compile_options(fpbody_tests PRIVATE /W4 /permissive-)
add_test(NAME fpbody_tests COMMAND fpbody_tests)

add_executable(vrinput_tests tests/vrinput_tests.cpp)
target_include_directories(vrinput_tests PRIVATE src)
target_compile_options(vrinput_tests PRIVATE /W4 /permissive-)
add_test(NAME vrinput_tests COMMAND vrinput_tests)

add_executable(steam_shadow_tests tests/steam_shadow_tests.cpp)
target_compile_options(steam_shadow_tests PRIVATE /W4 /permissive-)
add_test(NAME steam_shadow_tests COMMAND steam_shadow_tests)
