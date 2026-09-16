# Writes version.hpp with the current commit and whether the tree is dirty.
# Only rewrites the file when the content changes, so it does not force rebuilds.
execute_process(COMMAND git -C ${SRC} rev-parse --short=12 HEAD
  OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C ${SRC} status --porcelain --untracked-files=no
  OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(commit STREQUAL "")
  set(commit "unknown")
endif()
if(NOT dirty STREQUAL "")
  set(commit "${commit}-dirty")
endif()
set(content "#pragma once\n#define TTT_GIT_COMMIT \"${commit}\"\n")
if(EXISTS ${OUT})
  file(READ ${OUT} old)
else()
  set(old "")
endif()
if(NOT old STREQUAL content)
  file(WRITE ${OUT} "${content}")
endif()
