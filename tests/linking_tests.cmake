# Sharing by link sends gists through GitHub's command line; the tests' gh is
# a stand-in (tests/fakegh.cpp) that sends to the fake GitHub, in a folder of
# its own so that a test can put it first on PATH.
add_executable(fakegh tests/fakegh.cpp)
target_link_libraries(fakegh PRIVATE Qt6::Core Qt6::Network)
set_target_properties(fakegh PROPERTIES OUTPUT_NAME gh RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake-gh")
omabook_test_runs_omabook(${suite})
add_dependencies(${suite} fakegh)
target_compile_definitions(${suite} PRIVATE FAKE_GH="$<TARGET_FILE:fakegh>")
