# The Folio plugins are read from the source tree, as they are installed.
omabook_test_runs_omabook(${suite})
target_compile_definitions(${suite} PRIVATE OMABOOK_SOURCE="${CMAKE_SOURCE_DIR}" OMABOOK_TEST_TMP="${CMAKE_BINARY_DIR}")
