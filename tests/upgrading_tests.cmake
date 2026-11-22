# The first start after an update, with a plugin installed, runs the real binary (#60).
omabook_test_runs_omabook(${suite})
target_sources(${suite} PRIVATE tests/samples.cpp)
