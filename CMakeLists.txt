cmake_minimum_required(VERSION 3.21)

# Static vcpkg libraries + static CRT: one self-contained exe, nothing to install.
set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(wreckbox VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/W4 /utf-8 /permissive-)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS WB_VERSION="${PROJECT_VERSION}")

find_package(TagLib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)
find_package(GLEW REQUIRED)
find_package(projectM4 CONFIG REQUIRED COMPONENTS Playlist)
find_path(DRLIBS_INCLUDE_DIR dr_wav.h REQUIRED)

# Engine: decoding, analysis, tags. No UI, no globals.
add_library(wbengine STATIC
  src/engine/decode.cpp
  src/engine/analysis.cpp
  src/engine/tags.cpp
  src/engine/engine.cpp)
target_include_directories(wbengine PUBLIC src PRIVATE ${DRLIBS_INCLUDE_DIR})
target_link_libraries(wbengine PUBLIC nlohmann_json::nlohmann_json PRIVATE TagLib::tag mfplat mfreadwrite mfuuid)

# Model: library / state / analysis / settings JSON, paths. Same file formats as the original build.
add_library(wbmodel STATIC
  src/model/model.cpp
  src/model/paths.cpp
  src/model/settings.cpp)
target_include_directories(wbmodel PUBLIC src)
target_link_libraries(wbmodel PUBLIC nlohmann_json::nlohmann_json)

# Library: the store (state + scanning, analysing, organising, tagging), matcher, HTTP client.
add_library(wblibrary STATIC
  src/library/downloads_watcher.cpp
  src/library/matcher.cpp
  src/library/store.cpp
  src/net/account.cpp
  src/net/bug_report.cpp
  src/net/http_client.cpp
  src/net/oauth.cpp
  src/net/slsk/client.cpp
  src/net/slsk/match.cpp
  src/net/slsk/protocol.cpp
  src/net/slsk/sync.cpp
  src/net/soulseek.cpp
  src/net/sync_server.cpp
  src/net/tunnel.cpp
  src/net/updates.cpp
  src/sources/sources.cpp
  src/sources/csv_import.cpp
  src/sources/spotify.cpp
  src/sources/youtube.cpp
  src/sources/sync.cpp)
target_link_libraries(wblibrary PUBLIC wbengine wbmodel PRIVATE winhttp ws2_32 bcrypt shell32 iphlpapi normaliz httplib::httplib ZLIB::ZLIB)

# Player: VLC's engine (libVLC, downloaded by cmake/vlc.cmake) → miniaudio output, with a tap for the visualizer.
include(cmake/vlc.cmake)
include(cmake/milkdrop.cmake)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
add_library(wbplayer STATIC
  src/player/audio_output.cpp
  src/player/milkdrop.cpp
  src/player/vlc_engine.cpp
  src/player/player.cpp
  src/player/playlist.cpp
  src/player/visualizer.cpp)
target_include_directories(wbplayer PUBLIC src PRIVATE ${MINIAUDIO_INCLUDE_DIR})
target_link_libraries(wbplayer PUBLIC wblibrary libvlc PRIVATE libprojectM::projectM libprojectM::playlist GLEW::GLEW opengl32 gdi32)

# The app: Win32 window + Direct2D UI. Fonts and icon are embedded via res/app.rc.
add_executable(wreckbox WIN32
  src/ui/main.cpp
  src/ui/gfx.cpp
  src/ui/jobs.cpp
  src/ui/artwork.cpp
  src/ui/ui.cpp
  src/ui/view.cpp
  src/ui/view_tracks.cpp
  src/ui/view_settings.cpp
  src/ui/view_phone.cpp
  src/ui/view_extras.cpp
  src/ui/view_soulseek.cpp
  src/ui/screenshot.cpp
  src/ui/view_player.cpp
  src/ui/view_visualizer.cpp
  src/ui/media_controls.cpp
  res/app.rc
  res/app.manifest)
target_compile_definitions(wreckbox PRIVATE "WB_VER_COMMA=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
target_link_libraries(wreckbox PRIVATE wbplayer unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator d2d1 dwrite windowscodecs dwmapi shell32 ole32 comctl32 runtimeobject shlwapi shcore)
# libVLC loads on first use (the first play), not at startup: ~3 MB of DLLs off the cold-start path.
target_link_options(wreckbox PRIVATE /DELAYLOAD:libvlc.dll)
target_link_libraries(wreckbox PRIVATE delayimp)
vlc_deploy(wreckbox)
milkdrop_deploy(wreckbox)
# Licences next to the exe (Settings → About opens the folder): VLC's LGPL / GPL texts, the source link, the rest.
configure_file(res/licenses/NOTICE.txt.in ${CMAKE_BINARY_DIR}/NOTICE.txt @ONLY)
add_custom_command(TARGET wreckbox POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:wreckbox>/licenses
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/res/licenses/LGPL-2.1.txt ${CMAKE_SOURCE_DIR}/res/licenses/GPL-2.0.txt
          ${CMAKE_SOURCE_DIR}/res/Urbanist-OFL.txt ${CMAKE_SOURCE_DIR}/res/Doto-OFL.txt ${CMAKE_BINARY_DIR}/NOTICE.txt
          $<TARGET_FILE_DIR:wreckbox>/licenses)

add_executable(wbcore src/tools/wbcore.cpp)
target_link_libraries(wbcore PRIVATE wbengine)

enable_testing()
add_executable(engine_tests tests/engine_tests.cpp)
target_link_libraries(engine_tests PRIVATE wbengine)
add_test(NAME engine_tests COMMAND engine_tests)
add_executable(model_tests tests/model_tests.cpp)
target_link_libraries(model_tests PRIVATE wbmodel)
add_test(NAME model_tests COMMAND model_tests)
add_executable(library_tests tests/library_tests.cpp)
target_link_libraries(library_tests PRIVATE wblibrary)
add_test(NAME library_tests COMMAND library_tests)
add_executable(sources_tests tests/sources_tests.cpp)
target_link_libraries(sources_tests PRIVATE wblibrary)
target_compile_definitions(sources_tests PRIVATE WB_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME sources_tests COMMAND sources_tests)
add_executable(extras_tests tests/extras_tests.cpp)
target_link_libraries(extras_tests PRIVATE wblibrary httplib::httplib)
add_test(NAME extras_tests COMMAND extras_tests)
add_executable(slsk_tests tests/slsk_tests.cpp)
target_link_libraries(slsk_tests PRIVATE wblibrary httplib::httplib)
target_compile_definitions(slsk_tests PRIVATE WB_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME slsk_tests COMMAND slsk_tests)
add_executable(slsk_net_tests tests/slsk_net_tests.cpp)
target_link_libraries(slsk_net_tests PRIVATE wblibrary)
add_test(NAME slsk_net_tests COMMAND slsk_net_tests)
add_executable(slsk_sync_tests tests/slsk_sync_tests.cpp)
target_link_libraries(slsk_sync_tests PRIVATE wblibrary)
add_test(NAME slsk_sync_tests COMMAND slsk_sync_tests)
add_executable(soulseek_tests tests/soulseek_tests.cpp)
target_link_libraries(soulseek_tests PRIVATE wblibrary)
add_test(NAME soulseek_tests COMMAND soulseek_tests)
add_executable(sync_tests tests/sync_tests.cpp)
target_link_libraries(sync_tests PRIVATE wblibrary httplib::httplib)
add_test(NAME sync_tests COMMAND sync_tests)
add_executable(player_tests tests/player_tests.cpp)
target_link_libraries(player_tests PRIVATE wbplayer)
target_compile_definitions(player_tests PRIVATE WB_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
vlc_deploy(player_tests)
milkdrop_deploy(player_tests)
add_test(NAME player_tests COMMAND player_tests)
