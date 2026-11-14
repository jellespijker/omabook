# Omabook

## Building

Build with `bin/build` (or `bin/sanitize`), or pass `-j"$(bin/jobs)"` to
`cmake --build`. Never use a bare `-j` or `-j$(nproc)`: with the Makefile
generator a bare `-j` is unlimited, and on a low-memory machine the compile
thrashes until systemd-oomd kills the whole terminal session, agents included.
Run one build at a time; do not build several `build-*` directories in parallel.
