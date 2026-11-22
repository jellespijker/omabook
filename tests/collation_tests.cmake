# Names sort the same under a C locale (a bare container, a desktop set to
# C.UTF-8) as under a language's (#121): the tests that depend on it, again.
add_test(NAME collation_c_locale_tests COMMAND ${suite})
set_tests_properties(collation_c_locale_tests PROPERTIES ENVIRONMENT "${omabook_c_locale_environment}")
