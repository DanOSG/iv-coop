cmake_minimum_required(VERSION 3.24)
project(iv-coop LANGUAGES C CXX)

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
FetchContent_MakeAvailable(minhook imgui)

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

# --- the plugin ---
add_library(iv-coop SHARED
  src/dllmain.cpp
  src/core/log.cpp
  src/core/config.cpp
  src/core/game_version.cpp
  src/core/iat.cpp
  src/render/d3d9_hook.cpp
  src/render/overlay.cpp)
target_include_directories(iv-coop PRIVATE src)
target_compile_definitions(iv-coop PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(iv-coop PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(iv-coop PRIVATE minhook imgui d3d9 version)
set_target_properties(iv-coop PROPERTIES SUFFIX ".asi" OUTPUT_NAME "iv-coop")

if(IVCOOP_DEPLOY)
  add_custom_command(TARGET iv-coop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${IVCOOP_GAME_DIR}/plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:iv-coop>" "${IVCOOP_GAME_DIR}/plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:iv-coop>" "${IVCOOP_GAME_DIR}/plugins/"
    COMMENT "Deploying iv-coop.asi to ${IVCOOP_GAME_DIR}/plugins")
endif()
