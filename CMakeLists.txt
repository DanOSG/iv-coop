cmake_minimum_required(VERSION 3.24)
project(iv-coop VERSION 0.5.0 LANGUAGES C CXX)
add_compile_definitions(IVCOOP_VERSION="${PROJECT_VERSION}")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "GTA IV is a 32-bit game: configure with -A Win32 (use the 'x86' preset).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so players don't need a matching VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IVCOOP_GAME_DIR "C:/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto IV/GTAIV"
    CACHE PATH "GTA IV install folder (the one containing GTAIV.exe)")
option(IVCOOP_DEPLOY "Copy the built .asi into <game>/plugins after each build" ON)

# --- third-party (fetched; SOURCE_SUBDIR trick keeps their own CMake out of the build) ---
include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.3
  SOURCE_SUBDIR  _none)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9
  SOURCE_SUBDIR  _none)
FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG        v1.3.18
  SOURCE_SUBDIR  _none)
FetchContent_MakeAvailable(minhook imgui enet)

add_library(enet STATIC
  ${enet_SOURCE_DIR}/callbacks.c
  ${enet_SOURCE_DIR}/compress.c
  ${enet_SOURCE_DIR}/host.c
  ${enet_SOURCE_DIR}/list.c
  ${enet_SOURCE_DIR}/packet.c
  ${enet_SOURCE_DIR}/peer.c
  ${enet_SOURCE_DIR}/protocol.c
  ${enet_SOURCE_DIR}/win32.c)
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(enet PRIVATE /wd5287) # third-party enum mixing, harmless
target_link_libraries(enet PUBLIC ws2_32 winmm)

add_library(minhook STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

set(IVCOOP_WARNINGS /W4 /permissive- /utf-8)

# --- netcode shared by the plugin and the bot (no game dependencies) ---
add_library(ivc_net STATIC src/net/session.cpp)
target_include_directories(ivc_net PUBLIC src)
target_compile_definitions(ivc_net PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(ivc_net PRIVATE ${IVCOOP_WARNINGS})
target_link_libraries(ivc_net PUBLIC enet)

# --- headless test client ---
add_executable(iv-coop-bot tools/bot/main.cpp)
target_compile_options(iv-coop-bot PRIVATE ${IVCOOP_WARNINGS})
target_link_libraries(iv-coop-bot PRIVATE ivc_net)

# --- the plugin ---
add_library(iv-coop SHARED
  src/dllmain.cpp
  src/core/log.cpp
  src/core/config.cpp
  src/core/game_version.cpp
  src/core/iat.cpp
  src/core/instance.cpp
  src/core/crash.cpp
  src/core/focus.cpp
  src/game/pattern.cpp
  src/game/addresses.cpp
  src/game/natives.cpp
  src/game/script.cpp
  src/game/netprobe.cpp
  src/game/radio_probe.cpp
  src/game/radio.cpp
  src/game/player_slots.cpp
  src/game/phone.cpp
  src/net/steam.cpp
  src/net/steam_relay.cpp
  src/game/native_hooks.cpp
  src/story/call_log.cpp
  src/story/story_sync.cpp
  src/story/world_state.cpp
  src/story/dialogue.cpp
  src/story/sounds.cpp
  src/story/cutscene.cpp
  src/story/hud.cpp
  src/story/fx.cpp
  src/story/environment.cpp
  src/story/mission_audit.cpp
  src/story/coverage.cpp
  src/story/mission_runner.cpp
  src/sync/ghost.cpp
  src/sync/local_player.cpp
  src/sync/puppet.cpp
  src/sync/remote_players.cpp
  src/sync/vehicle_sync.cpp
  src/sync/downed.cpp
  src/sync/drunk_sync.cpp
  src/sync/radio_sync.cpp
  src/sync/session_settings.cpp
  src/sync/wallet.cpp
  src/sync/scream.cpp
  src/sync/attach_sync.cpp
  src/sync/ped_looks.cpp
  src/sync/team_fire.cpp
  src/sync/melee_sync.cpp
  src/sync/car_follower.cpp
  src/sync/entity_sync.cpp
  src/render/d3d9_hook.cpp
  src/render/overlay.cpp)
target_include_directories(iv-coop PRIVATE src)
target_compile_definitions(iv-coop PRIVATE UNICODE _UNICODE)
target_compile_options(iv-coop PRIVATE ${IVCOOP_WARNINGS})
target_link_libraries(iv-coop PRIVATE ivc_net minhook imgui d3d9 version)
set_target_properties(iv-coop PROPERTIES SUFFIX ".asi" OUTPUT_NAME "iv-coop")
# Linker map, so crash-logger offsets (iv-coop.asi+0x...) can be resolved to functions.
target_link_options(iv-coop PRIVATE /MAP:$<TARGET_FILE_DIR:iv-coop>/iv-coop.map)

if(IVCOOP_DEPLOY)
  # -D must precede -P for script mode.
  set(deploy_script -P ${CMAKE_SOURCE_DIR}/cmake/deploy.cmake)
  set(deploy_dst -DDST_DIR=${IVCOOP_GAME_DIR}/plugins)
  add_custom_command(TARGET iv-coop POST_BUILD
    COMMAND ${CMAKE_COMMAND} ${deploy_dst} -DSRC=$<TARGET_FILE:iv-coop> ${deploy_script}
    COMMAND ${CMAKE_COMMAND} ${deploy_dst} -DSRC=$<TARGET_PDB_FILE:iv-coop> ${deploy_script}
    COMMENT "Deploying iv-coop.asi to ${IVCOOP_GAME_DIR}/plugins (live on next launch if the game is running)")
endif()

# --- the installer: one exe with the mod embedded (see tools/installer/main.cpp) ---
add_executable(iv-coop-setup WIN32 tools/installer/main.cpp tools/installer/installer.rc)
target_compile_options(iv-coop-setup PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(iv-coop-setup PRIVATE winhttp bcrypt version comctl32 shell32 ole32)
target_link_options(iv-coop-setup PRIVATE
  "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
add_dependencies(iv-coop-setup iv-coop)
set_source_files_properties(tools/installer/installer.rc PROPERTIES
  COMPILE_DEFINITIONS "IVCOOP_ASI_PATH=\"$<TARGET_FILE:iv-coop>\""
  OBJECT_DEPENDS "$<TARGET_FILE:iv-coop>")
target_compile_definitions(iv-coop-setup PRIVATE UNICODE _UNICODE NOMINMAX)
