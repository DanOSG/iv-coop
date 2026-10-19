# Copies the built plugin into <game>/plugins, even while the game is running:
# a loaded DLL can't be overwritten on Windows but it can be renamed, so the old
# one is moved aside to *.old first and picked up for deletion on the next deploy.
#   cmake -DSRC=<file> -DDST_DIR=<dir> -P deploy.cmake
get_filename_component(name "${SRC}" NAME)
set(dst "${DST_DIR}/${name}")
file(MAKE_DIRECTORY "${DST_DIR}")
file(GLOB stale "${DST_DIR}/${name}.*.old")
foreach(f IN LISTS stale)
  file(REMOVE "${f}") # fails silently while still loaded; retried next time
endforeach()
if(EXISTS "${dst}")
  string(TIMESTAMP stamp "%Y%m%d%H%M%S")
  file(RENAME "${dst}" "${dst}.${stamp}.old" RESULT rename_result)
endif()
file(COPY_FILE "${SRC}" "${dst}")
