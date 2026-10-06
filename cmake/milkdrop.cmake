# MilkDrop preset packs for projectM: downloaded once as GitHub archives of pinned commits (an archive of a commit is
# stable, a branch is not), checked against their SHA-256 and unpacked into build/_deps/milkdrop. Gives
# milkdrop_deploy(<target>), which copies them to <exe dir>/milkdrop/{presets,textures}.

set(MILKDROP_DIR ${CMAKE_BINARY_DIR}/_deps/milkdrop)
set(MILKDROP_PACKS
  "presets|presets-cream-of-the-crop|0180df21f5e0bd39b9060cc5de420ed2f1f9e509|5600f974497d8f73295393fbdf866895ceb89efe82892f5e2d019358b2659202|"
  "textures|presets-milkdrop-texture-pack|6368812f27bc747b517218fbf89d21d59afce4d9|38e93612f79a313830480d7458d8eb065d2b67992211a5dd0c92ef87b8607f6a|textures")

foreach(pack ${MILKDROP_PACKS})
  string(REPLACE "|" ";" f "${pack}")
  list(GET f 0 name)
  list(GET f 1 repo)
  list(GET f 2 sha)
  list(GET f 3 hash)
  list(GET f 4 sub)
  if(NOT EXISTS ${MILKDROP_DIR}/${name}/.ok)
    message(STATUS "Downloading MilkDrop ${name} (${repo})")
    set(zip ${MILKDROP_DIR}/${name}.zip)
    file(DOWNLOAD https://github.com/projectM-visualizer/${repo}/archive/${sha}.zip ${zip}
         EXPECTED_HASH SHA256=${hash} SHOW_PROGRESS)
    file(REMOVE_RECURSE ${MILKDROP_DIR}/${name} ${MILKDROP_DIR}/x-${name})
    file(ARCHIVE_EXTRACT INPUT ${zip} DESTINATION ${MILKDROP_DIR}/x-${name})
    file(RENAME ${MILKDROP_DIR}/x-${name}/${repo}-${sha}/${sub} ${MILKDROP_DIR}/${name})
    file(REMOVE_RECURSE ${MILKDROP_DIR}/x-${name})
    file(WRITE ${MILKDROP_DIR}/${name}/.ok "")
  endif()
endforeach()

# The ~9,800 presets are copied only when the target's copy is missing (a stamp file), so incremental builds stay fast.
file(WRITE ${CMAKE_BINARY_DIR}/milkdrop_copy.cmake [[
if(NOT EXISTS "${DST}/.ok")
  file(MAKE_DIRECTORY "${DST}")
  file(COPY "${SRC}/" DESTINATION "${DST}")
endif()
]])

function(milkdrop_deploy target)
  foreach(name presets textures)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DSRC=${MILKDROP_DIR}/${name} -DDST=$<TARGET_FILE_DIR:${target}>/milkdrop/${name}
              -P ${CMAKE_BINARY_DIR}/milkdrop_copy.cmake
      VERBATIM)
  endforeach()
endfunction()
