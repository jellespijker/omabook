omabook_test_runs_omabook(${suite})
# A user for the test's own sshd and ssh, preloaded into them, where the
# container's user has no line in /etc/passwd (tests/hublink_passwd.cpp).
add_library(hublink_passwd MODULE tests/hublink_passwd.cpp)
target_link_libraries(hublink_passwd PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(${suite} hublink_passwd)
target_compile_definitions(${suite} PRIVATE HUBLINK_PASSWD="$<TARGET_FILE:hublink_passwd>")
