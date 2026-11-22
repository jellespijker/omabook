# Two copies of a book on one computer and one on the other, from the 0.1.5 sample, synced over the stand-in ssh (#58).
omabook_test_runs_omabook(${suite})
target_sources(${suite} PRIVATE tests/samples.cpp)
