# Two libraries brought up from 0.1.5 and synced over the stand-in ssh.
omabook_test_runs_omabook(${suite})
target_sources(${suite} PRIVATE tests/samples.cpp)
