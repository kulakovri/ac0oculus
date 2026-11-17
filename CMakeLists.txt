cmake_minimum_required(VERSION 3.20)
# The Quest 3 standalone build (aczquest fork, NOTES_QUEST.md; tools/quest/build_game.sh): libmain.so for an arm64
# NativeActivity APK, built by the NDK's clang. OFF (default) = the Windows build exactly as before. ON picks the NDK's
# toolchain file unless one is given, before project() reads it.
option(PS2_QUEST "build libmain.so for the Quest 3 APK (arm64 Android, NDK r29) instead of the Windows exe" OFF)
if(PS2_QUEST AND NOT CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ENV{ANDROID_NDK_ROOT})
    message(FATAL_ERROR "PS2_QUEST: ANDROID_NDK_ROOT is not set (QUEST_ENV: tools/quest/env.example.sh)")
  endif()
  file(TO_CMAKE_PATH "$ENV{ANDROID_NDK_ROOT}" PS2_QUEST_NDK)
  set(CMAKE_TOOLCHAIN_FILE "${PS2_QUEST_NDK}/build/cmake/android.toolchain.cmake" CACHE FILEPATH "the NDK's toolchain")
  set(ANDROID_ABI arm64-v8a CACHE STRING "")
  set(ANDROID_PLATFORM android-32 CACHE STRING "")
  set(ANDROID_STL c++_static CACHE STRING "")
endif()
project(ps2recomp_ac5 C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# -O1 on the PC build (GCC); the Quest build (clang) -O2: its EE thread is the device's limit (NOTES_QUEST S4), and on the
# PC clang's -O2 took the EE 3.50 -> 3.16 ms per field against its own -O1 (llvm-mingw, NOTES_ZERO_VR 2026-11-04 night_setup); arm64
# text +3.7 % (27.59 -> 28.60 MB), compile time about x2
if(PS2_QUEST)
  set(PS2_GEN_OPT "-O2" CACHE STRING "optimisation level of the generated code")
else()
  set(PS2_GEN_OPT "-O1" CACHE STRING "optimisation level of the generated code")
endif()
set(GEN_OPT "${PS2_GEN_OPT}")
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

option(PS2_DIAG "compile the entry histogram and the guest-store watch in" OFF)

find_package(Threads REQUIRED)

# The player's build (tools/setup/toolchains/*.cmake; night_setup 2026-11-04): a portable compiler with no MSYS2, no
# pkg-config and no Vulkan SDK. PS2_SDK_DIR holds the headers and import libraries of the libraries we ship
# (tools/setup/make_player_sdk.sh), PS2_PREBUILT_SPIRV the compiled shaders. All empty: the developer's build as before.
set(PS2_SDK_DIR "" CACHE PATH "headers and import libraries of SDL3, the OpenXR loader and vulkan-1 (the player's build)")
set(PS2_PREBUILT_SPIRV "" CACHE PATH "a folder of compiled .spv files: copied instead of running glslc (the player's build)")
option(PS2_BUILD_GSREPLAY "also build the gsreplay tool" ON)
# a release's objects name no folder of the machine that built them (__FILE__ in asserts; tools/setup/release_scan.py checks)
option(PS2_STRIP_PATHS "compile with -ffile-prefix-map: no absolute source paths in the binaries" OFF)
if(PS2_STRIP_PATHS)
  add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." "-ffile-prefix-map=${CMAKE_BINARY_DIR}=.")
endif()
if(PS2_QUEST)
  # No SDL library and no Vulkan SDK on the headset: the SDL3 headers declare the subset that runtime/src/android/
  # ps2_sdl_android.c implements over the NDK (AAudio, POSIX), and the Vulkan headers are newer than the NDK's (275:
  # no dynamic_rendering_local_read). Both are platform-neutral C headers, copied at configure time from a host
  # include folder (MSYS2's by default) into the build folder -- never the host folder itself on the include path.
  set(PS2_QUEST_HOST_INCLUDE "C:/msys64/ucrt64/include" CACHE PATH "a folder holding SDL3/, vulkan/ and vk_video/ headers")
  foreach(D SDL3 vulkan vk_video)
    if(NOT EXISTS "${PS2_QUEST_HOST_INCLUDE}/${D}")
      message(FATAL_ERROR "PS2_QUEST: ${PS2_QUEST_HOST_INCLUDE}/${D} not found (PS2_QUEST_HOST_INCLUDE)")
    endif()
    file(COPY "${PS2_QUEST_HOST_INCLUDE}/${D}" DESTINATION "${CMAKE_BINARY_DIR}/hostinc")
  endforeach()
  set(SDL3_INCLUDE_DIRS "${CMAKE_BINARY_DIR}/hostinc")
  set(SDL3_LIBRARY_DIRS "")
  set(SDL3_LIBRARIES "")
  set(Vulkan_INCLUDE_DIRS "${CMAKE_BINARY_DIR}/hostinc")
  add_library(Vulkan::Vulkan INTERFACE IMPORTED)
  set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_LINK_LIBRARIES vulkan)
  # the Khronos OpenXR loader for Android (the AAR unpacked: quest_sdk/openxr_loader_*)
  set(OPENXR_LOADER_DIR "$ENV{OPENXR_LOADER_DIR}" CACHE PATH "the unpacked Khronos openxr_loader_for_android AAR")
  file(TO_CMAKE_PATH "${OPENXR_LOADER_DIR}" OPENXR_LOADER_DIR)
  set(PS2_QUEST_XR_SO "${OPENXR_LOADER_DIR}/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so")
  if(NOT EXISTS "${PS2_QUEST_XR_SO}")
    message(FATAL_ERROR "PS2_QUEST: no ${PS2_QUEST_XR_SO} (OPENXR_LOADER_DIR)")
  endif()
  add_library(openxr_loader SHARED IMPORTED)
  set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION "${PS2_QUEST_XR_SO}" IMPORTED_NO_SONAME OFF
                        INTERFACE_INCLUDE_DIRECTORIES "${OPENXR_LOADER_DIR}/prefab/modules/headers/include")
  set(PS2_BUILD_GSREPLAY OFF CACHE BOOL "" FORCE)
  set(PS2_BUILD_AC0VR OFF CACHE BOOL "" FORCE)
elseif(PS2_SDK_DIR)
  set(SDL3_INCLUDE_DIRS "${PS2_SDK_DIR}/include")
  set(SDL3_LIBRARY_DIRS "${PS2_SDK_DIR}/lib")
  set(SDL3_LIBRARIES SDL3)
  set(Vulkan_INCLUDE_DIRS "${PS2_SDK_DIR}/include")
  add_library(Vulkan::Vulkan INTERFACE IMPORTED)
  set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_LINK_LIBRARIES vulkan-1)
else()
  find_package(Vulkan REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 REQUIRED sdl3)
endif()

set(PS2_GENERATED_DIR "generated" CACHE STRING
    "recompiler output directory, relative to the source tree")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/${PS2_GENERATED_DIR}")

if(NOT EXISTS "${GEN_DIR}/ps2_func_table.c")
  message(FATAL_ERROR
    "No generated code in ${PS2_GENERATED_DIR}/. Make it from your own disc with the recompiler (README, Build, step 2) "
    "and configure with -DPS2_GENERATED_DIR=gen_zero.")
endif()

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/ps2_code_*.c")
list(APPEND GENERATED_SOURCES
     "${GEN_DIR}/ps2_func_table.c"
     "${GEN_DIR}/ps2_image.c"
     "${GEN_DIR}/ps2_symbols.c")

set(RUNTIME_SOURCES
    runtime/src/ps2_core.c
    runtime/src/ps2_crash.c
    runtime/src/ps2_path.c
    runtime/src/ps2_version.c
    runtime/src/ps2_hostprof.c
    runtime/src/ps2_hw.c
    runtime/src/ps2_gfxq.c
    runtime/src/ps2_gs.c
    runtime/src/ps2_texrep.c
    runtime/src/zero_fmv.c
    runtime/src/ps2_vu.c
    runtime/src/ps2_vu0.c
    runtime/src/ps2_ipu.c
    runtime/src/ps2_kernel.c
    runtime/src/ps2_spu2.c
    runtime/src/ps2_spatial.c
    runtime/src/ps2_nustream.c
    runtime/src/ps2_memcard.c
    runtime/src/ps2_quit.c
    runtime/src/ps2_audio.c
    runtime/src/ps2_recexact.c
    runtime/src/ps2_gforce_audio.c
    runtime/src/ps2_sif.c
    runtime/src/ps2_vfs.c
    runtime/src/ps2_hle_test.c
    runtime/src/ps2_hle_sif.c
    runtime/src/ps2_hle_cdvd.c
    runtime/src/ps2_hle_pad.c
    runtime/src/ps2_touch.c
    runtime/src/ps2_hotas.c
    runtime/src/ps2_hle_iop.c
    runtime/src/ps2_video.c
    runtime/src/ps2_clouds.c
    runtime/src/ps2_clouds_far.c
    runtime/src/ps2_clouds_near.c
    runtime/src/ps2_clouds_shadow.c
    runtime/src/ps2_clouds_optics.c
    runtime/src/ps2_clouds_wake.c
    runtime/src/ps2_vkcen.c
    runtime/src/ps2_clouds_art.c
    runtime/src/ps2_trails.c
    runtime/src/ps2_cracks.c
    runtime/src/ps2_headvol.c
    runtime/src/ps2_state.c
    runtime/src/ps2_capture.c
    runtime/src/ps2_statecap.c
    runtime/src/ps2_settings.c
    runtime/src/ps2_stats.c
    runtime/src/ps2_telem.c
    runtime/src/ps2_mod.c
    runtime/src/ps2_hook.c
    runtime/src/ps2_modapi.c
    runtime/src/ps2_patch.c
    runtime/src/ps2_params.c
    runtime/src/ps2_lua.c
    runtime/src/ps2_ui.cpp
    runtime/src/ps2_launcher.cpp
    runtime/src/rn/rn_census.c
    runtime/src/rn/rn_2d.c
    runtime/src/rn/rn_dump.c
    runtime/src/rn/rn_vp.c
    runtime/src/rn/rn_vp_model.c
    runtime/src/rn/rn_vp_terrain.c
    runtime/src/rn/rn_vp_shadow.c
    runtime/src/rn/rn_vp_sprite.c
    runtime/src/rn/rn_vp_load.c
    runtime/src/rn/rn_vp_lit.c
    runtime/src/rn/rn_vp_fx.c
    runtime/src/rn/rn_vp_flat.c
    runtime/src/rn/rn_vpfuzz.c
    runtime/src/rn/rn_intent.c
    runtime/src/rn/rn_fixes.c
    runtime/src/rn/rn_sun.c
    runtime/src/rn/rn_sky.c
    runtime/src/rn/rn_tri3d.c
    runtime/src/rn/rn_billboard.c
    runtime/src/rn/rn_worldtri.c
    runtime/src/rn/rn_screen.c
    runtime/src/rn/rn_tap.c
    runtime/src/ps2_main.c)
# The Quest's ENHANCEMENTS table and QUEST SPEAKERS (rq2_menu; docs/SETTINGS.md "The Quest"): in the Quest build; in a PC build only
# with PS2_QUEST_SETTINGS=ON (tests: the settings test, a simulator run of the Quest's page). The PC's default objects stay as they are.
option(PS2_QUEST_SETTINGS "the Quest's ENHANCEMENTS table and the speaker profile in a PC build (tests)" OFF)
if(PS2_QUEST OR PS2_QUEST_SETTINGS)
  list(APPEND RUNTIME_SOURCES runtime/src/ps2_spk.c)
endif()
if(PS2_QUEST_SETTINGS AND NOT PS2_QUEST)
  add_compile_definitions(PS2_QUEST_SETTINGS=1)
endif()

set(LUA_DIR "${CMAKE_SOURCE_DIR}/third_party/lua")
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${LUA_DIR}/*.c")
add_library(lua54 STATIC ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC "${LUA_DIR}")
set_source_files_properties(${LUA_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(lua54 PRIVATE -O2)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
if(PS2_QUEST)
  # The first Quest build has no window toolkit: no Dear ImGui (the F4 page, the launcher), no SDL library. The
  # Android platform layer (android_main, commandline.txt, paths, logcat), the SDL3 subset over the NDK and the stubs of
  # the ImGui entry points take their place (runtime/src/android/, BUGS_QUEST.md for what the stubs lose).
  list(REMOVE_ITEM RUNTIME_SOURCES runtime/src/ps2_ui.cpp runtime/src/ps2_launcher.cpp)
  file(TO_CMAKE_PATH "${ANDROID_NDK}" PS2_QUEST_NDK_DIR)
  set(PS2_QUEST_GLUE "${PS2_QUEST_NDK_DIR}/sources/android/native_app_glue/android_native_app_glue.c")
  list(APPEND RUNTIME_SOURCES runtime/src/android/ps2_android.c runtime/src/android/ps2_sdl_android.c
                              runtime/src/android/ps2_quest_stubs.c runtime/src/android/ps2_vk13_android.c
                              runtime/src/android/ps2_pad_android.c runtime/src/android/ps2_display_android.c runtime/src/android/ps2_qrec_android.c
                              "${PS2_QUEST_GLUE}" third_party/miniz/miniz.c)
  set_source_files_properties("${PS2_QUEST_GLUE}" third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
  set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_DEFINITIONS
                              "MINIZ_NO_STDIO;MINIZ_NO_ARCHIVE_APIS;MINIZ_NO_TIME;MINIZ_NO_ZLIB_COMPATIBLE_NAMES")
  set_source_files_properties(runtime/src/ps2_main.c PROPERTIES COMPILE_DEFINITIONS "main=ps2_runtime_main")
else()
  list(APPEND RUNTIME_SOURCES ${IMGUI_SOURCES})
endif()

# Usage statistics (ps2_stats.c, docs/PRIVACY.md). posthog.secret at the tree's root (git-ignored, never exported): the first line
# the PostHog project token (write-only), the second the region, eu or us. Without the file the build has no key: it never asks
# and never sends. The token goes into a generated header, never onto a compile command line, and is never printed. The
# player's build (tools/setup/toolchain_build.sh, make_bundle.sh) compiles ps2_stats.c into libac5rt.a with it.
set(ACZ_STATS_SECRET_FILE "${CMAKE_SOURCE_DIR}/posthog.secret" CACHE FILEPATH "the PostHog project token and region")
set(ACZ_STATS_KEY_VALUE "")
set(ACZ_STATS_REGION_VALUE "eu")
if(EXISTS "${ACZ_STATS_SECRET_FILE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ACZ_STATS_SECRET_FILE}")
  file(STRINGS "${ACZ_STATS_SECRET_FILE}" ACZ_STATS_LINES LIMIT_COUNT 2)
  list(LENGTH ACZ_STATS_LINES ACZ_STATS_NLINES)
  if(ACZ_STATS_NLINES GREATER 0)
    list(GET ACZ_STATS_LINES 0 ACZ_STATS_KEY_VALUE)
    string(STRIP "${ACZ_STATS_KEY_VALUE}" ACZ_STATS_KEY_VALUE)
  endif()
  if(NOT ACZ_STATS_KEY_VALUE MATCHES "^[A-Za-z0-9_-]+$")
    set(ACZ_STATS_KEY_VALUE "")
    message(WARNING "posthog.secret: the first line is not a project token: this build has no statistics key")
  endif()
  if(ACZ_STATS_NLINES GREATER 1)
    list(GET ACZ_STATS_LINES 1 ACZ_STATS_REGION_VALUE)
    string(STRIP "${ACZ_STATS_REGION_VALUE}" ACZ_STATS_REGION_VALUE)
  else()
    message(WARNING "posthog.secret: no second line (eu or us): building for eu")
  endif()
  if(NOT ACZ_STATS_REGION_VALUE MATCHES "^(eu|us)$")
    message(WARNING "posthog.secret: the second line is not eu or us: building for eu")
    set(ACZ_STATS_REGION_VALUE "eu")
  endif()
  if(ACZ_STATS_KEY_VALUE)
    message(STATUS "Usage statistics: a project key found, region ${ACZ_STATS_REGION_VALUE}")
  endif()
else()
  message(STATUS "Usage statistics: no posthog.secret: this build never asks and never sends")
endif()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/stats_key/ps2_stats_key.h" CONTENT
"/* generated by CMakeLists.txt; never committed, never printed */
#define ACZ_STATS_KEY_DEFAULT \"@ACZ_STATS_KEY_VALUE@\"
#define ACZ_STATS_REGION_DEFAULT \"@ACZ_STATS_REGION_VALUE@\"
" @ONLY)

if(PS2_QUEST)
  add_library(ac5 SHARED ${RUNTIME_SOURCES} ${GENERATED_SOURCES})     # libmain.so (android.app.lib_name = main)
  set_target_properties(ac5 PROPERTIES OUTPUT_NAME main)
else()
  add_executable(ac5 ${RUNTIME_SOURCES} ${GENERATED_SOURCES})
endif()
target_sources(ac5 PRIVATE runtime/src/ps2_profile.c runtime/src/zero_vr.c runtime/src/zero_menus.c runtime/src/zero_audio3d.c runtime/src/zero_syssnd.c runtime/src/zero_clouds.c runtime/src/zero_terrain.c runtime/src/zero_terrain_mat.c runtime/src/zero_props.c runtime/src/zero_water.c runtime/src/zero_texpack.c runtime/src/zero_trails.c runtime/src/zero_cracks.c runtime/src/zero_flyby.c runtime/src/ps2_xr.c)
target_link_libraries(ac5 PRIVATE openxr_loader)
target_include_directories(ac5 PRIVATE "${CMAKE_BINARY_DIR}/stats_key" runtime/include "${GEN_DIR}"
                                       ${IMGUI_DIR} ${IMGUI_DIR}/backends
                                       ${SDL3_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS})
if(PS2_QUEST)
  # The headset: Vulkan, the OpenXR loader (shipped in the APK beside libmain.so), AAudio, logcat, the NDK's app glue.
  # -z max-page-size=16384: 16 KB pages (the NDK r28+ default, stated); --no-undefined: every symbol resolved here,
  # not at the first dlopen on the device; ANativeActivity_onCreate kept for NativeActivity (the glue defines it).
  target_include_directories(ac5 PRIVATE runtime/src/android "${PS2_QUEST_NDK_DIR}/sources/android/native_app_glue"
                                         third_party/sse2neon third_party/miniz)
  target_compile_definitions(ac5 PRIVATE PS2_QUEST=1)
  target_compile_options(ac5 PRIVATE $<$<COMPILE_LANGUAGE:C>:-fsigned-char -ffp-contract=off>
                                     -march=armv8.2-a+fp16+dotprod)
  # Hidden visibility (PS2_QUEST_HIDDEN, default ON): in a shared library every default-visibility function is
  # interposable, so each call between the 14,644 generated functions went through the PLT and every global (ps2_pt,
  # ps2_trace_ring, the hook bits) through the GOT -- 10,454 JUMP_SLOT relocations. Hidden, they are direct `bl` and
  # adrp+add. Only ANativeActivity_onCreate must stay exported: the NDK's glue does not mark it, so the glue file keeps
  # default visibility (build_game.sh checks the export).
  option(PS2_QUEST_HIDDEN "hidden symbol visibility in libmain.so (direct calls instead of PLT / GOT)" ON)
  if(PS2_QUEST_HIDDEN)
    target_compile_options(ac5 PRIVATE -fvisibility=hidden)
    target_compile_definitions(ac5 PRIVATE SDL_DECLSPEC=)        # (the SDL subset is ours: hidden like the rest)
    target_compile_options(lua54 PRIVATE -fvisibility=hidden)
    set_property(SOURCE "${PS2_QUEST_GLUE}" APPEND PROPERTY COMPILE_OPTIONS "-fvisibility=default")
  endif()
  # mediandk: ACZ_QUEST_RECORD's encoders and MP4 muxer (runtime/src/android/ps2_qrec_android.c)
  target_link_libraries(ac5 PRIVATE Threads::Threads m dl Vulkan::Vulkan lua54 aaudio log android mediandk)
  target_link_options(ac5 PRIVATE -Wl,-z,max-page-size=16384 -Wl,--no-undefined -u ANativeActivity_onCreate)
  target_compile_options(lua54 PRIVATE -fsigned-char -ffp-contract=off -march=armv8.2-a+fp16+dotprod)
else()
target_link_directories(ac5 PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(ac5 PRIVATE Threads::Threads m
                                  ${SDL3_LIBRARIES} Vulkan::Vulkan lua54 ws2_32 winhttp)
target_link_options(ac5 PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Soft hooks (ps2_runtime.h PS2_ENTER, ps2_hook.c): every generated function's entry tests a bit instead of carrying a
# patchable sled. ON in the Quest build (Android forbids patching an app's mapped code: SELinux execmod); on Windows
# OFF = the x86 detours as before (ON there is the PC test of the Quest's hook path).
if(PS2_QUEST)
  set(PS2_SOFT_HOOKS_DEFAULT ON)
  set(PS2_HOOK_ENTRIES_DEFAULT OFF)
else()
  set(PS2_SOFT_HOOKS_DEFAULT OFF)
  set(PS2_HOOK_ENTRIES_DEFAULT ON)
endif()
option(PS2_SOFT_HOOKS "hooks by a bit test at each generated function's entry instead of patched x86 sleds" ${PS2_SOFT_HOOKS_DEFAULT})
if(PS2_SOFT_HOOKS)
  target_compile_definitions(ac5 PRIVATE PS2_SOFT_HOOKS=1)
endif()
# The renderer without a window surface (ps2_video.c PS2_VK_HEADLESS): compiled into the Quest build; on Windows OFF =
# the window code exactly as before (ON there is the PC test of the Quest's renderer start, with PS2_VK_HEADLESS=1).
option(PS2_VK_HEADLESS_SUPPORT "compile the windowless renderer path (PS2_VK_HEADLESS)" ${PS2_SOFT_HOOKS_DEFAULT})
if(PS2_VK_HEADLESS_SUPPORT)
  target_compile_definitions(ac5 PRIVATE PS2_VK_HEADLESS_SUPPORT=1)
endif()
# The tile-friendly frame (ps2_video.c PS2_FRAME_GRAPH, quest_s2): compiled into the Quest build; on Windows OFF = the
# renderer exactly as before (ON there is the PC test of the device's path: PS2_FRAME_GRAPH=1, PS2_DATE_LOCAL=0)
# (only ps2_video.c reads it: a source property, for ac5 and gsreplay alike; the other objects stay as they are)
option(PS2_FRAME_GRAPH_SUPPORT "compile the frame-graph renderer path (PS2_FRAME_GRAPH)" ${PS2_SOFT_HOOKS_DEFAULT})
if(PS2_FRAME_GRAPH_SUPPORT)
  set_property(SOURCE runtime/src/ps2_video.c APPEND PROPERTY COMPILE_DEFINITIONS PS2_FRAME_GRAPH_SUPPORT=1)
endif()
# PS2_VK_STRICT_SUPPORT (quest bring-up): the Vulkan hardening of PS2_VK_STRICT compiled in (ON in the Quest build, whose
# android_main turns it on; OFF on Windows: ps2_video.c as before; ON there is the PC test, PS2_VK_STRICT=1)
option(PS2_VK_STRICT_SUPPORT "compile the Vulkan bring-up hardening (PS2_VK_STRICT)" ${PS2_SOFT_HOOKS_DEFAULT})
if(PS2_VK_STRICT_SUPPORT)
  set_property(SOURCE runtime/src/ps2_video.c APPEND PROPERTY COMPILE_DEFINITIONS PS2_VK_STRICT_SUPPORT=1)
endif()
# PS2_RQ_SUPPORT (the native Quest renderer, the README's "The Quest build"): the sources of
# runtime/src/rq/ and the hooks in ps2_video.c / ps2_xr.c compiled in, read at run time through PS2_RQ (default off).
# ON in the Quest build; OFF on Windows = every object exactly as before (ON there is the PC test of the renderer)
# (ps2_kernel.c / ps2_audio.c: PS2_XR_BG_PAUSE's field hold and mute, BUGS_RQ R-77)
option(PS2_RQ_SUPPORT "compile the native Quest renderer (PS2_RQ)" ${PS2_SOFT_HOOKS_DEFAULT})
if(PS2_RQ_SUPPORT)
  set_property(SOURCE runtime/src/ps2_video.c runtime/src/ps2_gs.c runtime/src/ps2_xr.c runtime/src/rn/rn_vp_terrain.c runtime/src/ps2_clouds.c runtime/src/ps2_clouds_far.c runtime/src/ps2_clouds_near.c runtime/src/ps2_clouds_shadow.c runtime/src/ps2_cracks.c runtime/src/ps2_hostprof.c runtime/src/ps2_kernel.c runtime/src/ps2_audio.c runtime/src/zero_water.c runtime/src/zero_vr.c runtime/src/zero_clouds.c runtime/src/ps2_hle_pad.c APPEND PROPERTY COMPILE_DEFINITIONS PS2_RQ_SUPPORT=1)
  # (ps2_hle_pad.c: rq10_artefacts' [test] PS2_PAD_PLAY_SYNC hook, ps2_hle.h PS2_RQ_PAD_SYNC; empty without PS2_RQ_SUPPORT)
  # R-240: every other source that includes ps2_clouds.h, whose ps2_cloud_env has PS2_RQ_SUPPORT-only fields (without the
  # define it is 8 bytes short there and ps2_cloud_env_read overran zero_terrain.c's copy into the zt_gold ring: free(0x1)).
  # PS2_RQ_BUILD (every source) arms ps2_clouds.h's #error for a source that misses this list
  set_property(SOURCE runtime/src/ps2_clouds_art.c runtime/src/ps2_clouds_optics.c runtime/src/ps2_clouds_wake.c runtime/src/zero_props.c runtime/src/zero_terrain.c runtime/src/zero_terrain_mat.c runtime/src/zero_trails.c APPEND PROPERTY COMPILE_DEFINITIONS PS2_RQ_SUPPORT=1)
  add_compile_definitions(PS2_RQ_BUILD=1)
  file(GLOB RQ_SOURCES CONFIGURE_DEPENDS runtime/src/rq/*.c)
  if(RQ_SOURCES)
    target_sources(ac5 PRIVATE ${RQ_SOURCES})
  endif()
endif()
option(PS2_HOOK_ENTRIES "patchable entry sleds in generated code, for mod hooks" ${PS2_HOOK_ENTRIES_DEFAULT})
# ps2_vu0_advance (runtime/include/ps2_vu0.h): ON inlines the VU0 flag pipeline's whole slot loop at every call site
# of the generated code (the build before 2026-10-24); OFF keeps only the pending test inline (ps2_vu0.c)
option(PS2_VU0_ADV_INLINE "inline the whole VU0 flag pipeline loop at every generated call site (old build)" OFF)
# -mcmodel=small on the generated code only: its extern data (ps2_pt, ps2_trace_ring, ps2_trace_pos, ps2_loop_ctr,
# ps2_zero_q) lives in this image, so RIP-relative access replaces the medium model's .refptr pointer loads
option(PS2_GEN_CMODEL_SMALL "compile the generated code with -mcmodel=small (no .refptr indirection)" OFF)
set(GEN_COMPILE_OPTIONS "${GEN_OPT}")
if(PS2_HOOK_ENTRIES)
  list(APPEND GEN_COMPILE_OPTIONS "-fpatchable-function-entry=8")
endif()
if(PS2_GEN_CMODEL_SMALL)
  list(APPEND GEN_COMPILE_OPTIONS "-mcmodel=small")
endif()
if(PS2_QUEST)
  # the NDK's toolchain adds -g to every build type: none for the 1M generated lines (symbols name their functions)
  list(APPEND GEN_COMPILE_OPTIONS "-g0")
endif()
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
                            COMPILE_OPTIONS "${GEN_COMPILE_OPTIONS}")

set(HOT_SOURCES runtime/src/ps2_vu.c runtime/src/ps2_gs.c runtime/src/ps2_hw.c)
set_source_files_properties(${HOT_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3")

# Every path in the runtime's C files is UTF-8 (runtime/include/ps2_path.h, setup_1105). Not ps2_vu.c: the player's PC compiles it
# without this flag and its dumps go to relative paths; not the generated code, third-party code or C++.
get_target_property(PS2_AC5_SOURCES ac5 SOURCES)
set(PS2_UTF8_SOURCES ${PS2_AC5_SOURCES} ${RUNTIME_SOURCES})
list(REMOVE_DUPLICATES PS2_UTF8_SOURCES)
list(FILTER PS2_UTF8_SOURCES INCLUDE REGEX "^runtime/src/.*[.]c$")
list(REMOVE_ITEM PS2_UTF8_SOURCES runtime/src/ps2_vu.c runtime/src/ps2_path.c runtime/src/ps2_version.c)
set_property(SOURCE ${PS2_UTF8_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-includeps2_path.h")   # (joined: GCC and clang both take it)

# The version (setup_1105): ps2_version.h in the build folder, from git at every build, written only when it changed; only
# runtime/src/ps2_version.c includes it. -DACZ_VERSION=0.1.0 sets it (make_release.sh), the fallback is 0.1.0-dev.
set(ACZ_VERSION "" CACHE STRING "the version string (empty: from git describe, else 0.1.0-dev)")
set(ACZ_VERSION_GIT "" CACHE STRING "the commit shown beside the version (empty: from git)")
add_custom_target(ps2_version_h
  COMMAND "${CMAKE_COMMAND}" "-DSRC=${CMAKE_SOURCE_DIR}" "-DOUT=${CMAKE_BINARY_DIR}/ps2_version.h" "-DACZ_VERSION=${ACZ_VERSION}"
          "-DACZ_VERSION_GIT=${ACZ_VERSION_GIT}" -P "${CMAKE_SOURCE_DIR}/tools/setup/write_version.cmake"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/ps2_version.h" VERBATIM)
set_property(SOURCE runtime/src/ps2_version.c APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}")
set_property(SOURCE runtime/src/ps2_version.c APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/ps2_version.h")
add_dependencies(ac5 ps2_version_h)

if(PS2_DIAG)
  target_compile_definitions(ac5 PRIVATE PS2_DIAG=1)
endif()
if(PS2_VU0_ADV_INLINE)
  target_compile_definitions(ac5 PRIVATE PS2_VU0_ADV_INLINE=1)
endif()

target_compile_options(ac5 PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

if(PS2_BUILD_GSREPLAY)
set(REPLAY_SOURCES ${RUNTIME_SOURCES})
list(REMOVE_ITEM REPLAY_SOURCES runtime/src/ps2_main.c
                                runtime/src/ps2_lua.c
                                runtime/src/ps2_modapi.c
                                runtime/src/ps2_mod.c
                                runtime/src/ps2_hook.c
                                runtime/src/ps2_patch.c
                                runtime/src/ps2_params.c
                                runtime/src/rn/rn_intent.c
                                runtime/src/rn/rn_fixes.c
                                runtime/src/rn/rn_tap.c)
list(APPEND REPLAY_SOURCES runtime/src/ps2_replay.c)

add_executable(gsreplay ${REPLAY_SOURCES} runtime/src/ps2_xr.c)
target_link_libraries(gsreplay PRIVATE openxr_loader)
target_include_directories(gsreplay PRIVATE runtime/include "${GEN_DIR}"
                                            "${LUA_DIR}"
                                            ${IMGUI_DIR} ${IMGUI_DIR}/backends
                                            ${SDL3_INCLUDE_DIRS}
                                            ${Vulkan_INCLUDE_DIRS})
target_link_directories(gsreplay PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(gsreplay PRIVATE Threads::Threads m
                                       ${SDL3_LIBRARIES} Vulkan::Vulkan ws2_32 winhttp)
target_compile_definitions(gsreplay PRIVATE ACZ_STATS_NO_KEY=1)
target_link_options(gsreplay PRIVATE -static-libstdc++ -static-libgcc)
if(PS2_DIAG)
  target_compile_definitions(gsreplay PRIVATE PS2_DIAG=1)
endif()
if(PS2_VU0_ADV_INLINE)
  target_compile_definitions(gsreplay PRIVATE PS2_VU0_ADV_INLINE=1)
endif()

add_dependencies(gsreplay ps2_version_h)

target_compile_options(gsreplay PRIVATE
    -fno-strict-aliasing -fwrapv
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()

set(SHADER_OUT "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
if(PS2_PREBUILT_SPIRV)
add_custom_target(shaders ALL
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS2_PREBUILT_SPIRV}" "${SHADER_OUT}"
  COMMENT "prebuilt SPIR-V from ${PS2_PREBUILT_SPIRV}")
else()
if(PS2_QUEST)
  # the NDK's own glslc, SPIR-V for Vulkan 1.1 (the headset's driver is 1.3; 1.1 keeps the modules to what every
  # shader here needs: multiview is core in 1.1)
  find_program(GLSLC glslc HINTS "${PS2_QUEST_NDK_DIR}/shader-tools/windows-x86_64" NO_DEFAULT_PATH)
  set(PS2_GLSLC_FLAGS --target-env=vulkan1.1)
else()
  set(PS2_GLSLC_FLAGS)
  find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
endif()
if(NOT GLSLC)
  message(FATAL_ERROR "glslc not found; it ships with the Vulkan SDK")
endif()

set(SPV_FILES)
# includes of the GS-path shaders (mesh.vert: the cloud shadow lookup for cloud shadows on the ground)
set(GS_GLSL_INC "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/terrain_water.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/terrain_detail.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/city_look.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/terrain_zcensus.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cut.glsl")
foreach(SH gs.vert gs.frag gs_zcopy.frag colclip.frag present.vert present.frag
           rn_vis.frag rn_sun.vert rn_sun.frag mesh.vert mesh.frag cockpit.vert cockpit.frag
           cockpit_shadow.vert cockpit_comp.vert cockpit_comp.frag cockpit_screen.frag cockpit_cut.frag edge_aa.frag
           cloudveil.vert cloudveil.frag gforce.vert gforce.frag trails.vert trails.frag haze.frag eye_adapt.frag
           burst.vert burst.frag
           xr_depth.frag pmask.vert mirterr.vert mirterr.frag quad_tblend.vert quad_tblend.frag)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${SH}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}"
            -o "${SHADER_OUT}/${SH}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}" ${GS_GLSL_INC}
    COMMENT "glslc ${SH}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${SH}.spv")
endforeach()
# multiview (single-pass stereo) variants of the GS shaders: same sources compiled with -DMV
foreach(PAIR "gs.vert;gs_mv.vert" "gs.frag;gs_mv.frag" "gs_zcopy.frag;gs_zcopy_mv.frag"
             "mesh.vert;mesh_mv.vert" "mesh.frag;mesh_mv.frag"
             "cockpit.vert;cockpit_mv.vert" "cockpit.frag;cockpit_mv.frag" "cockpit_comp.frag;cockpit_comp_mv.frag"
             "cockpit_screen.frag;cockpit_screen_mv.frag" "cockpit_cut.frag;cockpit_cut_mv.frag"
             "edge_aa.frag;edge_aa_mv.frag" "cloudveil.vert;cloudveil_mv.vert"
             "trails.vert;trails_mv.vert" "haze.frag;haze_mv.frag" "xr_depth.frag;xr_depth_mv.frag"
             "burst.vert;burst_mv.vert" "burst.frag;burst_mv.frag"
             "pmask.vert;pmask_mv.vert" "smooth_warp.frag;smooth_warp_mv.frag" "quad_tblend.frag;quad_tblend_mv.frag")
  list(GET PAIR 0 SRC)
  list(GET PAIR 1 DST)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DMV "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}" ${GS_GLSL_INC}
    COMMENT "glslc -DMV ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# shader water: the water variant of mesh.frag (terrain_water.glsl), used only by the terrain's water patches
# near-ground materials (terrain_detail.glsl): the terrain's first pass near the eye, also on water patches
foreach(TRIPLE "mesh.frag;mesh_wat.frag;-DWATER" "mesh.frag;mesh_wat_mv.frag;-DMV;-DWATER"
               "mesh.frag;mesh_det.frag;-DTDETAIL" "mesh.frag;mesh_det_mv.frag;-DMV;-DTDETAIL"
               "mesh.frag;mesh_watdet.frag;-DWATER;-DTDETAIL" "mesh.frag;mesh_watdet_mv.frag;-DMV;-DWATER;-DTDETAIL"
               # T5 terrain texture pack (zero_texpack.c): the terrain's first pass of the patches the pack serves
               "mesh.frag;mesh_tp.frag;-DTEXPACK" "mesh.frag;mesh_tp_mv.frag;-DMV;-DTEXPACK"
               "mesh.frag;mesh_wattp.frag;-DWATER;-DTEXPACK" "mesh.frag;mesh_wattp_mv.frag;-DMV;-DWATER;-DTEXPACK"
               "mesh.frag;mesh_dettp.frag;-DTDETAIL;-DTEXPACK" "mesh.frag;mesh_dettp_mv.frag;-DMV;-DTDETAIL;-DTEXPACK"
               "mesh.frag;mesh_watdettp.frag;-DWATER;-DTDETAIL;-DTEXPACK"
               "mesh.frag;mesh_watdettp_mv.frag;-DMV;-DWATER;-DTDETAIL;-DTEXPACK"
               # the city's detailed look (zero_props.c, city_look.glsl): only the city buildings' draws
               "mesh.frag;mesh_city.frag;-DCITY" "mesh.frag;mesh_city_mv.frag;-DMV;-DCITY"
               # perf5_terrain: the terrain overlay passes' zero-colour cull (PS2_PERF_TERRAIN_ZCULL) and the census of
               # their fragments (PS2_TERRAIN_ZCENSUS, diagnostic: terrain_zcensus.glsl)
               "mesh.vert;mesh_zc.vert;-DZCULL" "mesh.vert;mesh_zc_mv.vert;-DMV;-DZCULL"
               "mesh.frag;mesh_zcen.frag;-DZCENSUS" "mesh.frag;mesh_zcen_mv.frag;-DMV;-DZCENSUS"
               "gs.frag;gs_zcen.frag;-DZCENSUS" "gs.frag;gs_zcen_mv.frag;-DMV;-DZCENSUS"
               "gs.frag;gs_zcen_lr.frag;-DLOCAL_READ;-DZCENSUS" "gs.frag;gs_zcen_mv_lr.frag;-DMV;-DLOCAL_READ;-DZCENSUS")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}" ${GS_GLSL_INC}
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# PS2_RQ_SUPPORT (the native Quest renderer, R1): the per-view crop shaders and the feed pass
if(PS2_RQ_SUPPORT)
foreach(TRIPLE "gs.vert;gs_rq_mv.vert;-DMV;-DRQ" "mesh.vert;mesh_rq_mv.vert;-DMV;-DRQ"
               "mesh.vert;mesh_zc_rq_mv.vert;-DMV;-DRQ;-DZCULL" "mesh.vert;mesh_rq_b_mv.vert;-DMV;-DRQ;-DBATCH"
               "mesh.vert;mesh_zc_rq_b_mv.vert;-DMV;-DRQ;-DZCULL;-DBATCH" "gs.frag;gs_rq_mv.frag;-DMV;-DRQ"
               "gs.frag;gs_rq_mv_lr.frag;-DMV;-DRQ;-DLOCAL_READ;-DRQ_MS" "gs.frag;gs_rq_mv_lr_ns.frag;-DMV;-DRQ;-DLOCAL_READ;-DRQ_MS;-DNOSPEC" "gs.frag;gs_rq_mv_ns.frag;-DMV;-DRQ;-DNOSPEC" "gs.frag;gs_rq_mv_lr_df0.frag;-DMV;-DRQ;-DLOCAL_READ;-DRQ_MS;-DDATE_FORCE=0.0" "gs.frag;gs_rq_mv_lr_df1.frag;-DMV;-DRQ;-DLOCAL_READ;-DRQ_MS;-DDATE_FORCE=1.0" "rq_ds.vert;rq_ds.vert;-DRQ" "rq_ds.frag;rq_ds.frag;-DRQ" "rq_gs_rt.frag;rq_gs_rt.frag;-DRQ" "rq_hud_p.vert;rq_hud_p.vert;-DRQ" "rq_hud_p.frag;rq_hud_p.frag;-DRQ" "rq_gs_sky.frag;rq_gs_sky.frag;-DRQ" "rq_gs.frag;rq_gs_mp.frag;-DRQ;-DMP" "rq_gs.frag;rq_gs_date_mp.frag;-DRQ;-DDATE;-DMP" "rq_gs_flat.frag;rq_gs_flat_mp.frag;-DRQ;-DMP" "rq_layer.frag;rq_layer_cl_mp.frag;-DCL;-DMP" "rq_layer.frag;rq_layer_cp_mp.frag;-DCP;-DMP" "rq_ds.frag;rq_ds_nz.frag;-DRQ;-DNOZ" "rq_zpre.vert;rq_zpre.vert;-DRQ" "rq_zpre_ck.frag;rq_zpre_ck.frag;-DRQ" "rq_amask.frag;rq_amask.frag;-DRQ" "rq_cockpit.vert;rq_cockpit.vert;-DRQ" "rq_cockpit.frag;rq_cockpit.frag;-DRQ" "rq_cockpit.frag;rq_cockpit_mp.frag;-DRQ;-DRQC_MP" "rq_cockpit_sm.vert;rq_cockpit_sm.vert;-DRQ" "rq_cockpit_old.vert;rq_cockpit_old.vert;-DRQ" "rq_cockpit_glass.vert;rq_cockpit_glass.vert;-DRQ" "rq_cockpit_glass.frag;rq_cockpit_glass.frag;-DRQ" "rq_cockpit_glass.vert;rq_cockpit_wet.vert;-DRQ;-DWET" "rq_cockpit_wet.frag;rq_cockpit_wet.frag;-DRQ" "rq_cockpit_lwet.vert;rq_cockpit_lwet.vert;-DRQ" "rq_cockpit_lwet.frag;rq_cockpit_lwet.frag;-DRQ" "rq_cockpit_lwet.vert;rq_cockpit_lwet_vuv.vert;-DRQ;-DVUV" "rq_cockpit_lwet.frag;rq_cockpit_lwet_vuv.frag;-DRQ;-DVUV" "rq_cockpit_lwet.frag;rq_cockpit_lwet_ns.frag;-DRQ;-DNOSPEC" "rq_cockpit_lwet2.vert;rq_cockpit_lwet2.vert;-DRQ" "rq_cockpit_lwet2.frag;rq_cockpit_lwet2.frag;-DRQ" "rq_cockpit_lwet2.vert;rq_cockpit_lwet2_vuv.vert;-DRQ;-DVUV" "rq_cockpit_lwet2.frag;rq_cockpit_lwet2_vuv.frag;-DRQ;-DVUV" "rq_cockpit_fleck.frag;rq_cockpit_fleck.frag;-DRQ" "rq_cockpit_gx.vert;rq_cockpit_gx.vert;-DRQ" "rq_cockpit_gx.vert;rq_cockpit_gxw.vert;-DRQ;-DWEAR" "rq_cockpit_gx.frag;rq_cockpit_gx.frag;-DRQ" "rq_cockpit_gx.frag;rq_cockpit_gxw.frag;-DRQ;-DWEAR" "rq_cockpit_gx.vert;rq_cockpit_gx_rfs.vert;-DRQ;-DRFS" "rq_cockpit_gx.vert;rq_cockpit_gxw_rfs.vert;-DRQ;-DWEAR;-DRFS" "rq_cockpit_gx.frag;rq_cockpit_gx_rfs.frag;-DRQ;-DRFS" "rq_cockpit_gx.frag;rq_cockpit_gxw_rfs.frag;-DRQ;-DWEAR;-DRFS" "rq_glass_dust.vert;rq_glass_dust.vert;-DRQ" "rq_glass_dust.frag;rq_glass_dust.frag;-DRQ" "rq_glass_crk.vert;rq_glass_crk.vert;-DRQ" "rq_glass_crk.frag;rq_glass_crk.frag;-DRQ" "rq_burst.vert;rq_burst.vert;-DRQ" "rq_burst.frag;rq_burst.frag;-DRQ" "rq_clouds.vert;rq_clouds.vert;-DRQC" "rq_clouds.frag;rq_clouds.frag;-DRQC" "rq_clouds.frag;rq_clouds_fd.frag;-DRQC;-DFDEPTH" "rq_clouds.frag;rq_clouds_fs.frag;-DRQC;-DFSPLIT" "rq_clouds_near.vert;rq_clouds_near.vert;-DRQC" "rq_clouds_near.frag;rq_clouds_near.frag;-DRQC" "rq_clouds_near.vert;rq_clouds_near_e.vert;-DEDGE" "rq_clouds_near.frag;rq_clouds_near_e.frag;-DEDGE" "rq_hud.vert;rq_hud.vert;-DRQ" "rq_hud.frag;rq_hud.frag;-DRQ" "rq_gs.frag;rq_gs.frag;-DRQ" "rq_gs.frag;rq_gs_date.frag;-DRQ;-DDATE" "rq_gs.frag;rq_gs_date_df0.frag;-DRQ;-DDATE;-DDATE_FORCE=0.0" "rq_gs.frag;rq_gs_date_df1.frag;-DRQ;-DDATE;-DDATE_FORCE=1.0" "rq_gs_flat.frag;rq_gs_flat.frag;-DRQ" "rq_layer.frag;rq_layer_cl.frag;-DCL" "rq_layer.frag;rq_layer_cp.frag;-DCP" "rq_terrain.frag;rq_terrain_mv.frag;-DRQ" "rq_terrain.frag;rq_terrain_o_mv.frag;-DRQ;-DRQO" "rq_terrain.frag;rq_water_o_mv.frag;-DRQ;-DRQO;-DWATER"
               "rq_rec.frag;rq_rec.frag;-DRQ" "rq_testpat.frag;rq_testpat.frag;-DRQ" "rq_gs.frag;rq_gs_tree.frag;-DRQ;-DTREE_AA" "rq_gs.frag;rq_gs_tree_nm.frag;-DRQ;-DTREE_AA;-DTREE_NOMASK" "rq_gs.frag;rq_gs_tree_sd.frag;-DRQ;-DTREE_AA;-DTREE_SINGLE" "rq_gs.frag;rq_gs_tree_nm_mip.frag;-DRQ;-DTREE_AA;-DTREE_NOMASK;-DTREE_MIP" "rq_gs.frag;rq_gs_tree_sd_mip.frag;-DRQ;-DTREE_AA;-DTREE_SINGLE;-DTREE_MIP"
               "mesh.vert;mesh_rqt_b_mv.vert;-DMV;-DRQT;-DBATCH"
               "mesh.vert;mesh_zc_rqt_b_mv.vert;-DMV;-DZCULL;-DRQT;-DBATCH"
               "mesh.vert;mesh_rq_rqt_b_mv.vert;-DMV;-DRQ;-DRQT;-DBATCH"
               "mesh.vert;mesh_zc_rq_rqt_b_mv.vert;-DMV;-DRQ;-DZCULL;-DRQT;-DBATCH"
               "rq_terrain.frag;rq_terrain_b_mv.frag;-DRQ;-DBATCH"
               "rq_terrain.frag;rq_terrain_o_b_mv.frag;-DRQ;-DBATCH;-DRQO"
               "rq_terrain.frag;rq_water_o_b_mv.frag;-DRQ;-DBATCH;-DRQO;-DWATER"
               "mesh.frag;mesh_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH"
               "mesh.frag;mesh_wat_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DWATER"
               "mesh.frag;mesh_det_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DTDETAIL"
               "mesh.frag;mesh_watdet_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DWATER;-DTDETAIL"
               "mesh.frag;mesh_tp_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DTEXPACK"
               "mesh.frag;mesh_wattp_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DWATER;-DTEXPACK"
               "mesh.frag;mesh_dettp_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DTDETAIL;-DTEXPACK"
               "mesh.frag;mesh_watdettp_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DWATER;-DTDETAIL;-DTEXPACK"
               "mesh.frag;mesh_city_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DCITY"
               "mesh.vert;mesh_rqt_mv.vert;-DMV;-DRQT"
               "mesh.vert;mesh_zc_rqt_mv.vert;-DMV;-DZCULL;-DRQT"
               "mesh.vert;mesh_rq_rqt_mv.vert;-DMV;-DRQ;-DRQT"
               "mesh.vert;mesh_zc_rq_rqt_mv.vert;-DMV;-DRQ;-DZCULL;-DRQT"
               "mesh.frag;mesh_rqt_mv.frag;-DMV;-DRQT"
               "mesh.frag;mesh_wat_rqt_mv.frag;-DMV;-DRQT;-DWATER"
               "mesh.frag;mesh_det_rqt_mv.frag;-DMV;-DRQT;-DTDETAIL"
               "mesh.frag;mesh_watdet_rqt_mv.frag;-DMV;-DRQT;-DWATER;-DTDETAIL"
               "mesh.frag;mesh_tp_rqt_mv.frag;-DMV;-DRQT;-DTEXPACK"
               "mesh.frag;mesh_wattp_rqt_mv.frag;-DMV;-DRQT;-DWATER;-DTEXPACK"
               "mesh.frag;mesh_dettp_rqt_mv.frag;-DMV;-DRQT;-DTDETAIL;-DTEXPACK"
               "mesh.frag;mesh_watdettp_rqt_mv.frag;-DMV;-DRQT;-DWATER;-DTDETAIL;-DTEXPACK"
               "mesh.frag;mesh_city_rqt_mv.frag;-DMV;-DRQT;-DCITY")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}" ${GS_GLSL_INC}
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# rq13_cut, PS2_RQ_CLOUDS_CUT (prompt 12 Part A, BUGS_RQ R-190): the eye pass's twins that apply the cloud in front of each object
# (rq_cut.glsl, -DRQCUT): cut_<name>.spv beside each <name>.spv; rq_video.inc rq_load_shaders takes them in place of the originals while
# the switch is on (the originals stay byte-identical)
foreach(TRIPLE
               "mesh.vert;cut_mesh_rqt_mv.vert;-DMV;-DRQT;-DRQCUT" "mesh.vert;cut_mesh_zc_rqt_mv.vert;-DMV;-DZCULL;-DRQT;-DRQCUT"
               "mesh.vert;cut_mesh_rqt_b_mv.vert;-DMV;-DRQT;-DBATCH;-DRQCUT" "mesh.vert;cut_mesh_zc_rqt_b_mv.vert;-DMV;-DZCULL;-DRQT;-DBATCH;-DRQCUT"
               "mesh.vert;cut_mesh_rq_mv.vert;-DMV;-DRQ;-DRQCUT"
               "mesh.vert;cut_mesh_zc_rq_mv.vert;-DMV;-DRQ;-DZCULL;-DRQCUT"
               "mesh.vert;cut_mesh_rq_b_mv.vert;-DMV;-DRQ;-DBATCH;-DRQCUT"
               "mesh.vert;cut_mesh_zc_rq_b_mv.vert;-DMV;-DRQ;-DZCULL;-DBATCH;-DRQCUT"
               "mesh.vert;cut_mesh_rq_rqt_mv.vert;-DMV;-DRQ;-DRQT;-DRQCUT"
               "mesh.vert;cut_mesh_zc_rq_rqt_mv.vert;-DMV;-DRQ;-DZCULL;-DRQT;-DRQCUT"
               "mesh.vert;cut_mesh_rq_rqt_b_mv.vert;-DMV;-DRQ;-DRQT;-DBATCH;-DRQCUT"
               "mesh.vert;cut_mesh_zc_rq_rqt_b_mv.vert;-DMV;-DRQ;-DZCULL;-DRQT;-DBATCH;-DRQCUT"
               "gs.vert;cut_gs_rq_mv.vert;-DMV;-DRQ;-DRQCUT"
               "mesh.frag;cut_mesh_mv.frag;-DMV;-DRQCUT"
               "mesh.frag;cut_mesh_b_mv.frag;-DMV;-DBATCH;-DRQCUT"
               "rq_gs.frag;cut_rq_gs.frag;-DRQ;-DRQCUT" "rq_gs.frag;cut_rq_gs_tree.frag;-DRQ;-DTREE_AA;-DRQCUT" "rq_gs.frag;cut_rq_gs_tree_nm.frag;-DRQ;-DTREE_AA;-DTREE_NOMASK;-DRQCUT" "rq_gs.frag;cut_rq_gs_tree_sd.frag;-DRQ;-DTREE_AA;-DTREE_SINGLE;-DRQCUT" "rq_gs.frag;cut_rq_gs_tree_nm_mip.frag;-DRQ;-DTREE_AA;-DTREE_NOMASK;-DTREE_MIP;-DRQCUT" "rq_gs.frag;cut_rq_gs_tree_sd_mip.frag;-DRQ;-DTREE_AA;-DTREE_SINGLE;-DTREE_MIP;-DRQCUT"
               "rq_gs.frag;cut_rq_gs_date.frag;-DRQ;-DDATE;-DRQCUT"
               "rq_gs.frag;cut_rq_gs_date_df0.frag;-DRQ;-DDATE;-DDATE_FORCE=0.0;-DRQCUT"
               "rq_gs.frag;cut_rq_gs_date_df1.frag;-DRQ;-DDATE;-DDATE_FORCE=1.0;-DRQCUT"
               "rq_gs_flat.frag;cut_rq_gs_flat.frag;-DRQ;-DRQCUT"
               "rq_terrain.frag;cut_rq_terrain_mv.frag;-DRQ;-DRQCUT"
               "rq_terrain.frag;cut_rq_terrain_o_mv.frag;-DRQ;-DRQO;-DRQCUT"
               "rq_terrain.frag;cut_rq_water_o_mv.frag;-DRQ;-DRQO;-DWATER;-DRQCUT"
               "rq_terrain.frag;cut_rq_terrain_b_mv.frag;-DRQ;-DBATCH;-DRQCUT"
               "rq_terrain.frag;cut_rq_terrain_o_b_mv.frag;-DRQ;-DBATCH;-DRQO;-DRQCUT"
               "rq_terrain.frag;cut_rq_water_o_b_mv.frag;-DRQ;-DBATCH;-DRQO;-DWATER;-DRQCUT"
               "mesh.frag;cut_mesh_rqt_mv.frag;-DMV;-DRQT;-DRQCUT"
               "mesh.frag;cut_mesh_wat_rqt_mv.frag;-DMV;-DRQT;-DWATER;-DRQCUT"
               "mesh.frag;cut_mesh_det_rqt_mv.frag;-DMV;-DRQT;-DTDETAIL;-DRQCUT"
               "mesh.frag;cut_mesh_watdet_rqt_mv.frag;-DMV;-DRQT;-DWATER;-DTDETAIL;-DRQCUT"
               "mesh.frag;cut_mesh_tp_rqt_mv.frag;-DMV;-DRQT;-DTEXPACK;-DRQCUT"
               "mesh.frag;cut_mesh_wattp_rqt_mv.frag;-DMV;-DRQT;-DWATER;-DTEXPACK;-DRQCUT"
               "mesh.frag;cut_mesh_dettp_rqt_mv.frag;-DMV;-DRQT;-DTDETAIL;-DTEXPACK;-DRQCUT"
               "mesh.frag;cut_mesh_watdettp_rqt_mv.frag;-DMV;-DRQT;-DWATER;-DTDETAIL;-DTEXPACK;-DRQCUT"
               "mesh.frag;cut_mesh_city_rqt_mv.frag;-DMV;-DRQT;-DCITY;-DRQCUT"
               "mesh.frag;cut_mesh_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DRQCUT"
               "mesh.frag;cut_mesh_wat_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DWATER;-DRQCUT"
               "mesh.frag;cut_mesh_det_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DTDETAIL;-DRQCUT"
               "mesh.frag;cut_mesh_watdet_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DWATER;-DTDETAIL;-DRQCUT"
               "mesh.frag;cut_mesh_tp_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DTEXPACK;-DRQCUT"
               "mesh.frag;cut_mesh_wattp_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DWATER;-DTEXPACK;-DRQCUT"
               "mesh.frag;cut_mesh_dettp_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DTDETAIL;-DTEXPACK;-DRQCUT"
               "mesh.frag;cut_mesh_watdettp_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DWATER;-DTDETAIL;-DTEXPACK;-DRQCUT"
               "mesh.frag;cut_mesh_city_rqt_b_mv.frag;-DMV;-DRQT;-DBATCH;-DCITY;-DRQCUT")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}" ${GS_GLSL_INC}
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
endif()
# quest S3b, PS2_RN_BATCH: the batched native-mesh variants (-DBATCH: the vertices and each draw's push-constant head read
# from storage buffers), one per mesh shader the batched draws can take; ps2_video.c rnb_load names them <base>_b[_mv]
foreach(TRIPLE "mesh.vert;mesh_b.vert;-DBATCH" "mesh.vert;mesh_b_mv.vert;-DMV;-DBATCH"
               "mesh.vert;mesh_zc_b.vert;-DZCULL;-DBATCH" "mesh.vert;mesh_zc_b_mv.vert;-DMV;-DZCULL;-DBATCH"
               "mesh.frag;mesh_b.frag;-DBATCH" "mesh.frag;mesh_b_mv.frag;-DMV;-DBATCH"
               "mesh.frag;mesh_wat_b.frag;-DWATER;-DBATCH" "mesh.frag;mesh_wat_b_mv.frag;-DMV;-DWATER;-DBATCH"
               "mesh.frag;mesh_det_b.frag;-DTDETAIL;-DBATCH" "mesh.frag;mesh_det_b_mv.frag;-DMV;-DTDETAIL;-DBATCH"
               "mesh.frag;mesh_watdet_b.frag;-DWATER;-DTDETAIL;-DBATCH"
               "mesh.frag;mesh_watdet_b_mv.frag;-DMV;-DWATER;-DTDETAIL;-DBATCH"
               "mesh.frag;mesh_tp_b.frag;-DTEXPACK;-DBATCH" "mesh.frag;mesh_tp_b_mv.frag;-DMV;-DTEXPACK;-DBATCH"
               "mesh.frag;mesh_wattp_b.frag;-DWATER;-DTEXPACK;-DBATCH"
               "mesh.frag;mesh_wattp_b_mv.frag;-DMV;-DWATER;-DTEXPACK;-DBATCH"
               "mesh.frag;mesh_dettp_b.frag;-DTDETAIL;-DTEXPACK;-DBATCH"
               "mesh.frag;mesh_dettp_b_mv.frag;-DMV;-DTDETAIL;-DTEXPACK;-DBATCH"
               "mesh.frag;mesh_watdettp_b.frag;-DWATER;-DTDETAIL;-DTEXPACK;-DBATCH"
               "mesh.frag;mesh_watdettp_b_mv.frag;-DMV;-DWATER;-DTDETAIL;-DTEXPACK;-DBATCH"
               "mesh.frag;mesh_city_b.frag;-DCITY;-DBATCH" "mesh.frag;mesh_city_b_mv.frag;-DMV;-DCITY;-DBATCH")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}" ${GS_GLSL_INC}
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# DATE through dynamic rendering local read (input attachment instead of a target snapshot); the cockpit's vertex
# shader with the primitive shading rate of ACZ_VR_COCKPIT_VRS (perf4_cockpit); PS2_FRAME_GRAPH's variant whose channel
# shuffle reads its own pixel in place too (gs_fg_*: -DFG_SELF; the gs_lr / gs_mv_lr SPIR-V stays the same)
foreach(TRIPLE "gs.frag;gs_lr.frag;-DLOCAL_READ" "gs.frag;gs_mv_lr.frag;-DMV;-DLOCAL_READ"
               "gs.frag;gs_fg_lr.frag;-DLOCAL_READ;-DFG_SELF" "gs.frag;gs_fg_mv_lr.frag;-DMV;-DLOCAL_READ;-DFG_SELF"
               "cockpit.vert;cockpit_vrs.vert;-DCPIT_VRS" "cockpit.vert;cockpit_vrs_mv.vert;-DMV;-DCPIT_VRS")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# the canopy glass's variant module (PS2_PERF_GLASS_VARIANTS, perf5_glassexact): cockpit.frag with -DGXV, specialised per
# glass state (the plain cockpit[_mv].frag.spv stays the same SPIR-V)
foreach(TRIPLE "cockpit.frag;cockpit_gx.frag;-DGXV" "cockpit.frag;cockpit_gx_mv.frag;-DMV;-DGXV")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# (S2c, PS2_VK_STRICT_SUPPORT) the packed 3D cockpit for devices that bind fewer than 12 descriptor sets (the Quest 3: 7):
# cockpit.frag with -DCPIT_PACKED, plain and glass variant (ps2_video.c cpit_pk)
if(PS2_VK_STRICT_SUPPORT)
  foreach(TRIPLE "cockpit.frag;cockpit_pk.frag;-DCPIT_PACKED" "cockpit.frag;cockpit_pk_mv.frag;-DMV;-DCPIT_PACKED"
                 "cockpit.frag;cockpit_gx_pk.frag;-DGXV;-DCPIT_PACKED" "cockpit.frag;cockpit_gx_pk_mv.frag;-DMV;-DGXV;-DCPIT_PACKED")
    list(GET TRIPLE 0 SRC)
    list(GET TRIPLE 1 DST)
    list(SUBLIST TRIPLE 2 -1 DEFS)
    add_custom_command(
      OUTPUT "${SHADER_OUT}/${DST}.spv"
      COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
              -o "${SHADER_OUT}/${DST}.spv"
      DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
      COMMENT "glslc ${DEFS} ${SRC}")
    list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
  endforeach()
endif()
# clouds (ps2_clouds.c, ACZ_VR_CLOUDS): compute passes and the composite; all include cloud_common.glsl
set(CLOUD_GLSL "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_common.glsl" "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.glsl"
    "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_optics.glsl")
# (cloud_optics.comp: the optics probe, ps2_clouds_optics.c; cloud_near_reuse.comp: ACZ_VR_CLOUD_NEAR_REUSE, ps2_clouds_near.c;
#  cloud_wake.comp: the cloud wake's probe, ps2_clouds_wake.c)
foreach(SH cloud_noise.comp cloud_weather.comp cloud_near.comp cloud_near_reuse.comp cloud_test.comp cloud_comp.frag cloud_optics.comp
        cloud_shafts.comp
        cloud_lgrid.comp
        cloud_wxocc.comp
        cloud_wake.comp)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${SH}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}" -o "${SHADER_OUT}/${SH}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}" ${CLOUD_GLSL}
    COMMENT "glslc ${SH}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${SH}.spv")
endforeach()
# the near march's half-precision variant (ACZ_VR_CLOUD_NEAR_FP16, Quest session 2026-11-08; ps2_clouds_near.c)
add_custom_command(
  OUTPUT "${SHADER_OUT}/cloud_near_h.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DNEAR_FP16 "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_near.comp" -o "${SHADER_OUT}/cloud_near_h.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_near.comp" ${CLOUD_GLSL}
  COMMENT "glslc cloud_near.comp (NEAR_FP16)")
list(APPEND SPV_FILES "${SHADER_OUT}/cloud_near_h.comp.spv")
# S2's far renderer: one source, a variant per pass (cloud_far.glsl is S2's include)
foreach(PASS weather wsoft tiles args march)
  string(TOUPPER "${PASS}" PASS_UP)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/cloud_far_${PASS}.comp.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DPASS_${PASS_UP} "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.comp"
            -o "${SHADER_OUT}/cloud_far_${PASS}.comp.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.comp" "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.glsl"
            ${CLOUD_GLSL}
    COMMENT "glslc -DPASS_${PASS_UP} cloud_far.comp")
  list(APPEND SPV_FILES "${SHADER_OUT}/cloud_far_${PASS}.comp.spv")
endforeach()
if(PS2_RQ_SUPPORT)
# PS2_RQ_CLOUDS (rq_clouds.inc): the Quest eye pass's far scheduler and march (one source, a variant per pass) and its near march
foreach(PASS tiles args march copy resolve)   # (copy: PS2_RQ_CLOUDS_FARSWAP; resolve: clouds7's PS2_RQ_CLOUDS_FARSHOWN)
  string(TOUPPER "${PASS}" PASS_UP)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/rq_cloud_far_${PASS}.comp.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DPASS_${PASS_UP} "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.comp"
            -o "${SHADER_OUT}/rq_cloud_far_${PASS}.comp.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.comp" "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.glsl"
            ${CLOUD_GLSL}
    COMMENT "glslc -DPASS_${PASS_UP} rq_cloud_far.comp")
  list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_far_${PASS}.comp.spv")
endforeach()
# (clouds7, PS2_RQ_CLOUDS_SPLAT_CULL) the weather splat culled per 8 x 8 texels and cached per texel: the Quest path's variant
add_custom_command(
  OUTPUT "${SHADER_OUT}/rq_cloud_far_weather.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DPASS_WEATHER -DRQ_SPLAT_CULL "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.comp"
          -o "${SHADER_OUT}/rq_cloud_far_weather.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.comp" "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.glsl" ${CLOUD_GLSL}
  COMMENT "glslc -DPASS_WEATHER -DRQ_SPLAT_CULL cloud_far.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_far_weather.comp.spv")
# [test] PS2_RQ_CLOUDS_FARSTUB (clouds6 2026-11-11): the march with sample counters (cnt) and the stub march, every density and the
# fine sample's light from one light-grid read (stub); the default rq_cloud_far_march.comp.spv is built without either define
# (lg: ACZ_VR_CLOUD_LGRID_DENS, clouds6 A.4 piece 5: the samples within the density grid's radius are one read of it)
# (bake / bakecnt: rq7_bake's PS2_RQ_CLOUDS_BAKE, every sample from the baked volumes; bakecnt with the read counters)
foreach(V cnt stub lg bake bakecnt bakechk)
  if(V STREQUAL "stub")
    set(RQ_FAR_VDEF -DCL_FAR_COUNT -DCL_FAR_STUB)
  elseif(V STREQUAL "lg")
    set(RQ_FAR_VDEF -DCL_FAR_LG)
  elseif(V STREQUAL "bake")
    set(RQ_FAR_VDEF -DCL_FAR_BAKE)
  elseif(V STREQUAL "bakecnt")
    set(RQ_FAR_VDEF -DCL_FAR_BAKE -DCL_FAR_COUNT)
  elseif(V STREQUAL "bakechk")
    set(RQ_FAR_VDEF -DCL_FAR_BAKE -DCL_FAR_CHECK)
  else()
    set(RQ_FAR_VDEF -DCL_FAR_COUNT)
  endif()
  add_custom_command(
    OUTPUT "${SHADER_OUT}/rq_cloud_far_march_${V}.comp.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DPASS_MARCH ${RQ_FAR_VDEF} "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.comp"
            -o "${SHADER_OUT}/rq_cloud_far_march_${V}.comp.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.comp" "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.glsl"
            ${CLOUD_GLSL}
    COMMENT "glslc -DPASS_MARCH ${RQ_FAR_VDEF} rq_cloud_far.comp")
  list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_far_march_${V}.comp.spv")
endforeach()
# PS2_RQ_CLOUDS_BAKE (rq7_bake 2026-11-14, prompt 9 steps 2-3): the volumes the far march reads, baked once (one source, a variant per pass)
foreach(PASS d l df)
  string(TOUPPER "${PASS}" PASS_UP)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/rq_cloud_bake_${PASS}.comp.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DPASS_BAKE_${PASS_UP} "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_bake.comp"
            -o "${SHADER_OUT}/rq_cloud_bake_${PASS}.comp.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_bake.comp" "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.glsl"
            ${CLOUD_GLSL}
    COMMENT "glslc -DPASS_BAKE_${PASS_UP} rq_cloud_bake.comp")
  list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_bake_${PASS}.comp.spv")
endforeach()
# PS2_RQ_CLOUDS_ONE=2 (rq8_onelayer 2026-11-14, prompt 10 step 3): the weather map covers the cloud list's period; the shadow map's build places
# its own 20 km torus by the cloud-frame origin (cloud_shadow.comp -DRQ_SH20; the plain module unchanged)
add_custom_command(
  OUTPUT "${SHADER_OUT}/rq_cloud_shadow.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DRQ_SH20 "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.comp" -o "${SHADER_OUT}/rq_cloud_shadow.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.comp" ${CLOUD_GLSL}
  COMMENT "glslc -DRQ_SH20 cloud_shadow.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_shadow.comp.spv")
# PS2_RQ_CLOUDS_FARCOARSE (clouds6 2026-11-11, A.4 P1c): a new generation's coarse level, one ray per tile corner (with the density
# grid's cached samples when ACZ_VR_CLOUD_LGRID_DENS is on)
add_custom_command(
  OUTPUT "${SHADER_OUT}/rq_cloud_far_coarse.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DPASS_COARSE -DCL_FAR_LG "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.comp"
          -o "${SHADER_OUT}/rq_cloud_far_coarse.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.comp" "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_far.glsl"
          ${CLOUD_GLSL}
  COMMENT "glslc -DPASS_COARSE -DCL_FAR_LG rq_cloud_far.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_far_coarse.comp.spv")
add_custom_command(
  OUTPUT "${SHADER_OUT}/rq_cloud_near.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_near.comp" -o "${SHADER_OUT}/rq_cloud_near.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_near.comp" ${CLOUD_GLSL}
  COMMENT "glslc rq_cloud_near.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_near.comp.spv")
# ACZ_VR_CLOUD_RQ_NEARFS: the near march as a fragment pass, and the in-cloud probe alone as compute
add_custom_command(
  OUTPUT "${SHADER_OUT}/rq_cloud_near_fs.frag.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -fshader-stage=frag -DNEAR_FS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_near.comp" -o "${SHADER_OUT}/rq_cloud_near_fs.frag.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_near.comp" ${CLOUD_GLSL}
  COMMENT "glslc -DNEAR_FS rq_cloud_near.comp")
add_custom_command(
  OUTPUT "${SHADER_OUT}/rq_cloud_near_probe.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DNEAR_PROBE_ONLY "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_near.comp" -o "${SHADER_OUT}/rq_cloud_near_probe.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_near.comp" ${CLOUD_GLSL}
  COMMENT "glslc -DNEAR_PROBE_ONLY rq_cloud_near.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_near_fs.frag.spv" "${SHADER_OUT}/rq_cloud_near_probe.comp.spv")
foreach(PASS A B)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/rq_cloud_nearmask_${PASS}.comp.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DPASS_${PASS} "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_nearmask.comp" -o "${SHADER_OUT}/rq_cloud_nearmask_${PASS}.comp.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_nearmask.comp" ${CLOUD_GLSL}
    COMMENT "glslc -DPASS_${PASS} rq_cloud_nearmask.comp")
  list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_nearmask_${PASS}.comp.spv")
endforeach()
# PS2_RQ_CLOUDS_FXFADE (R-55, rq_fxfade.inc): the game's effects fade per vertex by our cloud in front of them
add_custom_command(
  OUTPUT "${SHADER_OUT}/rq_cloud_fxfade.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_fxfade.comp" -o "${SHADER_OUT}/rq_cloud_fxfade.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_cloud_fxfade.comp" ${CLOUD_GLSL}
  COMMENT "glslc rq_cloud_fxfade.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rq_cloud_fxfade.comp.spv")
# PS2_RQ_TREE_AA bit 16 (R-242, rq_treemip.inc): the tree impostors' premultiplied mip chains
add_custom_command(
  OUTPUT "${SHADER_OUT}/rq_treemip.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_treemip.comp" -o "${SHADER_OUT}/rq_treemip.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rq_treemip.comp"
  COMMENT "glslc rq_treemip.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rq_treemip.comp.spv")
endif()
# the cloud shadow map (ps2_clouds_shadow.c): slice build and blend in one dispatch
add_custom_command(
  OUTPUT "${SHADER_OUT}/cloud_shadow.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.comp" -o "${SHADER_OUT}/cloud_shadow.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.comp" ${CLOUD_GLSL}
  COMMENT "glslc cloud_shadow.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/cloud_shadow.comp.spv")
add_custom_command(
  OUTPUT "${SHADER_OUT}/cloud_comp_mv.frag.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O -DMV "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_comp.frag" -o "${SHADER_OUT}/cloud_comp_mv.frag.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_comp.frag" ${CLOUD_GLSL}
  COMMENT "glslc -DMV cloud_comp.frag")
list(APPEND SPV_FILES "${SHADER_OUT}/cloud_comp_mv.frag.spv")
# the cloud depth stamp for the game's effect sprites (ACZ_VR_CLOUD_FX, bug #46): cloud_comp.frag with -DSTAMP;
# its soft replacement, the effects' cloud veil (ACZ_VR_CLOUD_FX_SOFT, bug #254): -DVEIL
foreach(TRIPLE "cloud_stamp.frag;-DSTAMP" "cloud_stamp_mv.frag;-DSTAMP;-DMV" "cloud_veil.frag;-DVEIL" "cloud_veil_mv.frag;-DVEIL;-DMV")
  list(GET TRIPLE 0 DST)
  list(SUBLIST TRIPLE 1 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_comp.frag" -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_comp.frag" ${CLOUD_GLSL}
    COMMENT "glslc cloud_comp.frag -> ${DST}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# the frame-exact recording's NV12 conversion of the window image (ACZ_REC_EXACT, ps2_video.c rx_*)
add_custom_command(
  OUTPUT "${SHADER_OUT}/rx_nv12.comp.spv"
  COMMAND "${GLSLC}" ${PS2_GLSLC_FLAGS} -O "${CMAKE_SOURCE_DIR}/runtime/shaders/rx_nv12.comp" -o "${SHADER_OUT}/rx_nv12.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rx_nv12.comp"
  COMMENT "glslc rx_nv12.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rx_nv12.comp.spv")
add_custom_target(shaders ALL DEPENDS ${SPV_FILES})
endif()
add_dependencies(ac5 shaders)
if(PS2_BUILD_GSREPLAY)
  add_dependencies(gsreplay shaders)
endif()

include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c23")
check_c_source_compiles("
void g(void){}
void f(void){ [[gnu::musttail]] return g(); }
int main(void){ f(); return 0; }" HAVE_MUSTTAIL)
if(NOT HAVE_MUSTTAIL)
  message(WARNING
    "This compiler has no musttail attribute. Cross-function guest loops will "
    "consume stack. GCC 15+ or Clang 13+ is required for a correct build.")
endif()

# ac0vr.exe (setup_1105): the player's entry point and the shortcut's target, shipped prebuilt: the first-run dialog, the build's progress
# (tools/setup/ac0vr_launcher.c). Win32 only, no SDL, no game code. The updater comes in when its files are in the tree.
option(PS2_BUILD_AC0VR "build ac0vr.exe, the player's launcher" ON)
if(WIN32 AND PS2_BUILD_AC0VR)
  set(AC0VR_SOURCES tools/setup/ac0vr_launcher.c)
  include(CheckLanguage)
  check_language(RC)
  if(CMAKE_RC_COMPILER)
    enable_language(RC)
    list(APPEND AC0VR_SOURCES tools/setup/ac0vr.rc)
  endif()
  set(AC0VR_LIBS ole32 uuid comctl32 comdlg32 shell32 gdi32)
  set(AC0VR_DEFS "")
  if(EXISTS "${CMAKE_SOURCE_DIR}/tools/setup/ac0vr_update.c")           # (ac0vr_launcher.c includes it)
    list(APPEND AC0VR_SOURCES third_party/miniz/miniz.c)
    set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
    list(APPEND AC0VR_LIBS winhttp shlwapi)
    set(AC0VR_DEFS MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
  endif()
  add_executable(ac0vr WIN32 ${AC0VR_SOURCES})
  target_compile_definitions(ac0vr PRIVATE ${AC0VR_DEFS})
  target_include_directories(ac0vr PRIVATE tools/setup third_party/miniz)
  target_compile_options(ac0vr PRIVATE $<$<COMPILE_LANGUAGE:C>:-Os -municode>)
  target_link_options(ac0vr PRIVATE -municode -s -static-libgcc)
  target_link_libraries(ac0vr PRIVATE ${AC0VR_LIBS})
endif()
