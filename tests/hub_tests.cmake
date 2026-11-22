omabook_test_runs_omabook(${suite})
# The systemd units, as the package installs them.
target_compile_definitions(${suite} PRIVATE OMABOOK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
