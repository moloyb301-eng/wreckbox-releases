# libVLC (VLC's playback engine) from the official VideoLAN Windows release: downloaded once, checked against its
# published SHA-256, unpacked into build/_deps. vcpkg has no libVLC port. Gives the imported target `libvlc` and
# vlc_deploy(<target>), which copies libvlc.dll, libvlccore.dll and the plugin subset in cmake/vlc_plugins.txt next to
# the target's exe (plugins in "plugins\", where libVLC looks next to libvlccore.dll) and rebuilds VLC's plugin cache.

set(VLC_VERSION 3.0.24)
set(VLC_SHA256 1ed59c09152e78aff84663fa76efab26057716576db74332858810e5ef54ae1f)
set(VLC_ARCHIVE ${CMAKE_BINARY_DIR}/_deps/vlc-${VLC_VERSION}-win64.7z)
set(VLC_ROOT ${CMAKE_BINARY_DIR}/_deps/vlc/vlc-${VLC_VERSION})

if(NOT EXISTS ${VLC_ROOT}/sdk/include/vlc/vlc.h)
  message(STATUS "Downloading libVLC ${VLC_VERSION}")
  file(DOWNLOAD https://download.videolan.org/pub/videolan/vlc/${VLC_VERSION}/win64/vlc-${VLC_VERSION}-win64.7z
       ${VLC_ARCHIVE} EXPECTED_HASH SHA256=${VLC_SHA256} SHOW_PROGRESS)
  file(ARCHIVE_EXTRACT INPUT ${VLC_ARCHIVE} DESTINATION ${CMAKE_BINARY_DIR}/_deps/vlc)
endif()

# The SDK's libvlc.lib comes from the MinGW toolchain, and MSVC can't delay-load through it. Make a native import
# library from the DLL's own export table (dumpbin + lib sit next to cl.exe), so the app can load libVLC on first play.
set(VLC_IMPLIB ${CMAKE_BINARY_DIR}/_deps/libvlc-msvc.lib)
if(NOT EXISTS ${VLC_IMPLIB})
  get_filename_component(MSVC_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
  execute_process(COMMAND ${MSVC_BIN}/dumpbin.exe /nologo /exports ${VLC_ROOT}/libvlc.dll OUTPUT_VARIABLE exports COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-F]+ [0-9A-F]+ (libvlc_[A-Za-z0-9_]+)" lines "${exports}")
  set(def "LIBRARY libvlc.dll\nEXPORTS\n")
  foreach(line ${lines})
    string(REGEX REPLACE ".* (libvlc_[A-Za-z0-9_]+)$" "\\1" name "${line}")
    string(APPEND def "  ${name}\n")
  endforeach()
  file(WRITE ${CMAKE_BINARY_DIR}/_deps/libvlc.def "${def}")
  execute_process(COMMAND ${MSVC_BIN}/lib.exe /nologo /machine:x64 /def:${CMAKE_BINARY_DIR}/_deps/libvlc.def /out:${VLC_IMPLIB}
                  COMMAND_ERROR_IS_FATAL ANY)
endif()

add_library(libvlc SHARED IMPORTED GLOBAL)
set_target_properties(libvlc PROPERTIES
  IMPORTED_LOCATION ${VLC_ROOT}/libvlc.dll
  IMPORTED_IMPLIB ${VLC_IMPLIB}
  INTERFACE_INCLUDE_DIRECTORIES ${VLC_ROOT}/sdk/include)

file(STRINGS ${CMAKE_SOURCE_DIR}/cmake/vlc_plugins.txt VLC_PLUGINS REGEX "^[^#].*\\.dll$")

function(vlc_deploy target)
  set(cmds)
  set(dirs)
  foreach(p ${VLC_PLUGINS})
    get_filename_component(dir ${p} DIRECTORY)
    list(APPEND dirs $<TARGET_FILE_DIR:${target}>/plugins/${dir})
    list(APPEND cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VLC_ROOT}/plugins/${p} $<TARGET_FILE_DIR:${target}>/plugins/${dir}/)
  endforeach()
  list(REMOVE_DUPLICATES dirs)
  set(cmds COMMAND ${CMAKE_COMMAND} -E make_directory ${dirs} ${cmds})
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VLC_ROOT}/libvlc.dll ${VLC_ROOT}/libvlccore.dll $<TARGET_FILE_DIR:${target}>
    ${cmds}
    # VLC scans its plugins at startup unless a cache lists them; regenerate it for our subset.
    COMMAND ${VLC_ROOT}/vlc-cache-gen.exe $<TARGET_FILE_DIR:${target}>/plugins
    VERBATIM)
endfunction()
