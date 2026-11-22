# The test suites are found by name, never listed: every tests/<name>_tests.cpp
# is a suite of its own, so adding one edits no shared file (#198). What a suite
# needs beyond the others (another source, a definition, a second run of it)
# is in tests/<name>_tests.cmake, included with the suite's target as ${suite}.
# Two that several use:
#   omabook_test_runs_omabook(${suite})  the suite runs this build's omabook: as
#     the other computer of a sync behind a stand-in ssh, or as a person and an
#     agent meet its commands and MCP tools
#   target_sources(${suite} PRIVATE tests/samples.cpp)  the library samples of
#     every release, for the tests of the change log and of syncing through it

# The names of the suites in `folder` (each <name>_tests), sorted.
function(omabook_test_suites out folder)
  # A new suite makes the build configure again (not in cmake -P, the tests' own).
  set(again CONFIGURE_DEPENDS)
  if(CMAKE_SCRIPT_MODE_FILE)
    set(again "")
  endif()
  file(GLOB sources ${again} "${folder}/*_tests.cpp")
  set(names "")
  foreach(source IN LISTS sources)
    get_filename_component(name "${source}" NAME_WE)
    list(APPEND names "${name}")
  endforeach()
  list(SORT names)
  set(${out} "${names}" PARENT_SCOPE)
endfunction()

function(omabook_test_runs_omabook suite)
  add_dependencies(${suite} omabook)
  target_compile_definitions(${suite} PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
endfunction()
