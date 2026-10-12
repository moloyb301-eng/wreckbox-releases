# DasVis's shaders, compiled at build time with the Windows SDK's fxc for Shader Model 4 (Direct3D feature level 10_0,
# so they run on old graphics cards). Each becomes a header with its bytecode, built into the exe: nothing to ship or
# load. A shader that doesn't compile fails the build. dasvis.hlsli is also embedded as text, for the user's own scenes.
# Gives DASVIS_GENERATED: the headers, to add to the library's sources.

set(_sdk_bin "C:/Program Files (x86)/Windows Kits/10/bin")
find_program(FXC fxc REQUIRED HINTS
  "${_sdk_bin}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  "$ENV{WindowsSdkVerBinPath}/x64"
  "${_sdk_bin}/x64")

set(DASVIS_SHADERS ${CMAKE_SOURCE_DIR}/shaders/dasvis)
set(DASVIS_GEN ${CMAKE_BINARY_DIR}/dasvis_gen)
file(MAKE_DIRECTORY ${DASVIS_GEN})
set(DASVIS_GENERATED "")

# name: the C array is k_<name>; file: relative to shaders/dasvis; profile: vs_4_0 / ps_4_0; then the entry point, if
# it isn't main (one file can hold a scene and its simulation's passes).
function(dasvis_shader name file profile)
  set(entry main)
  if(ARGC GREATER 3)
    set(entry ${ARGV3})
  endif()
  set(out ${DASVIS_GEN}/${name}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${FXC} /nologo /O3 /WX /T ${profile} /E ${entry} /Vn k_${name} /Fh ${out} /I ${DASVIS_SHADERS} ${DASVIS_SHADERS}/${file}
    DEPENDS ${DASVIS_SHADERS}/${file} ${DASVIS_SHADERS}/dasvis.hlsli
    COMMENT "fxc ${file}"
    VERBATIM)
  set(DASVIS_GENERATED ${DASVIS_GENERATED} ${out} PARENT_SCOPE)
endfunction()

dasvis_shader(vs_fullscreen fullscreen_vs.hlsl vs_4_0)
dasvis_shader(ps_feedback scenes/feedback.hlsl ps_4_0)
dasvis_shader(ps_tunnel scenes/tunnel.hlsl ps_4_0)
dasvis_shader(ps_kaleido scenes/kaleido.hlsl ps_4_0)
dasvis_shader(ps_ink scenes/ink.hlsl ps_4_0)
dasvis_shader(ps_ink_velocity scenes/ink.hlsl ps_4_0 ps_velocity)
dasvis_shader(ps_ink_divergence scenes/ink.hlsl ps_4_0 ps_divergence)
dasvis_shader(ps_ink_jacobi scenes/ink.hlsl ps_4_0 ps_jacobi)
dasvis_shader(ps_ink_gradient scenes/ink.hlsl ps_4_0 ps_gradient)
dasvis_shader(ps_coral scenes/coral.hlsl ps_4_0)
dasvis_shader(ps_coral_step scenes/coral.hlsl ps_4_0 ps_step)
dasvis_shader(ps_galaxy scenes/galaxy.hlsl ps_4_0)
dasvis_shader(vs_star scenes/galaxy.hlsl vs_4_0 vs_star)
dasvis_shader(ps_star scenes/galaxy.hlsl ps_4_0 ps_star)
dasvis_shader(ps_ridges scenes/ridges.hlsl ps_4_0)
dasvis_shader(vs_ridge scenes/ridges.hlsl vs_4_0 vs_ridge)
dasvis_shader(ps_ridge scenes/ridges.hlsl ps_4_0 ps_ridge)
dasvis_shader(ps_synthwave scenes/synthwave.hlsl ps_4_0)
dasvis_shader(ps_hyperspace scenes/hyperspace.hlsl ps_4_0)
dasvis_shader(ps_aurora scenes/aurora.hlsl ps_4_0)
dasvis_shader(ps_mandelbox scenes/mandelbox.hlsl ps_4_0)
dasvis_shader(ps_chrome scenes/chrome.hlsl ps_4_0)
dasvis_shader(ps_storm scenes/storm.hlsl ps_4_0)
dasvis_shader(ps_milkdrop scenes/milkdrop.hlsl ps_4_0)
dasvis_shader(vs_ribbon sims/ribbon.hlsl vs_4_0 vs_ribbon)
dasvis_shader(ps_ribbon sims/ribbon.hlsl ps_4_0 ps_ribbon)
dasvis_shader(ps_particles_update sims/particles.hlsl ps_4_0 ps_update)
dasvis_shader(vs_particles sims/particles.hlsl vs_4_0 vs_draw)
dasvis_shader(ps_particles sims/particles.hlsl ps_4_0 ps_draw)
dasvis_shader(ps_bloom_down post/bloom_down.hlsl ps_4_0)
dasvis_shader(ps_bloom_up post/bloom_up.hlsl ps_4_0)
dasvis_shader(ps_lum post/lum.hlsl ps_4_0)
dasvis_shader(ps_final post/final.hlsl ps_4_0)
dasvis_shader(ps_copy post/copy.hlsl ps_4_0)

# dasvis.hlsli as a C++ raw string (MSVC caps one literal at ~16 KB, so it's split into 8 KB pieces).
file(READ ${DASVIS_SHADERS}/dasvis.hlsli _lib)
string(LENGTH "${_lib}" _len)
set(_pieces "")
set(_at 0)
while(_at LESS _len)
  string(SUBSTRING "${_lib}" ${_at} 8000 _piece)
  string(APPEND _pieces "R\"HLSL(${_piece})HLSL\"\n")
  math(EXPR _at "${_at} + 8000")
endwhile()
file(WRITE ${DASVIS_GEN}/library.h.tmp "// Generated from shaders/dasvis/dasvis.hlsli.\ninline const char k_library[] =\n${_pieces};\n")
configure_file(${DASVIS_GEN}/library.h.tmp ${DASVIS_GEN}/library.h COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DASVIS_SHADERS}/dasvis.hlsli)
list(APPEND DASVIS_GENERATED ${DASVIS_GEN}/library.h)
