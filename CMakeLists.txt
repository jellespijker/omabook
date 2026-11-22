cmake_minimum_required(VERSION 3.24)
project(omabook VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# clang-tidy, clangd and the harness's checks read the compile flags from here.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Sql Concurrent Test Multimedia)
# OpenAI's voices after /audio/speech speak over its Realtime API, a WebSocket (#169).
find_package(Qt6 REQUIRED COMPONENTS WebSockets)
# The ink surface's tests send pen and touch events as the window system does
# (#78); a highlighter's line is triangulated with Qt's own triangulator (#130).
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
find_package(PkgConfig REQUIRED)
# libarchive opens EPUB, CBZ, CBR and friends; Poppler renders PDF; libsodium
# keeps a person's key pair and seals what a shared space sends (ADR 0001).
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(LIBSODIUM REQUIRED IMPORTED_TARGET libsodium)
# Recordings are kept as Opus (#79); Qt Multimedia hears and plays them.
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB omabook_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM omabook_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(omabook_core STATIC ${omabook_sources})
target_include_directories(omabook_core PUBLIC src)
target_link_libraries(omabook_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus Qt6::Sql Qt6::Concurrent Qt6::Multimedia Qt6::WebSockets
  PkgConfig::LIBARCHIVE PkgConfig::POPPLER_QT6 PkgConfig::LIBSODIUM PkgConfig::OPUS)
target_link_libraries(omabook_core PRIVATE Qt6::GuiPrivate)
target_compile_options(omabook_core PUBLIC -Wall -Wextra)
target_compile_definitions(omabook_core PUBLIC OMABOOK_VERSION="${PROJECT_VERSION}")
# The standard library's own checks (error() on an expected holding a value, an
# index past the end): Arch's makepkg builds the package with them (#57), so
# every build here, the tests too, does as well. Defining it again is harmless.
target_compile_definitions(omabook_core PUBLIC _GLIBCXX_ASSERTIONS)
# The Calibre bridge is a separate, GPL-licensed file (see README, License):
# installed to share/omabook, and found in the source tree by an uninstalled build.
target_compile_definitions(omabook_core PRIVATE OMABOOK_BRIDGE_SOURCE="${CMAKE_SOURCE_DIR}/share/omabook/calibre_bridge.py")

# Omarchy's theme palettes, compiled in (share/omabook/themes/README).
set(omabook_themes ${CMAKE_SOURCE_DIR}/share/omabook/themes/themes.qrc)
add_executable(omabook src/main.cpp src/resources.qrc ${omabook_themes})
# Whole: files that only register something when the program starts (the MCP
# tools in mcpcommands.cpp) are referenced by nothing, and the linker would
# leave them out of a plain static link. Needs CMake 3.24.
target_link_libraries(omabook PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,omabook_core>")

# The tests load the real Main.qml, so they carry the resources too. One
# binary per layer: a broken format reader fails without the window.
enable_testing()
# Every test runs in a home of its own: a test, or a command one runs, that
# opens "the library in use" finds this folder's config, never the
# developer's own library.
set(omabook_test_home "${CMAKE_BINARY_DIR}/test-home")
file(MAKE_DIRECTORY "${omabook_test_home}/.config" "${omabook_test_home}/.local/share" "${omabook_test_home}/.cache")
set(omabook_test_environment "QT_QPA_PLATFORM=offscreen;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;HOME=${omabook_test_home};XDG_CONFIG_HOME=${omabook_test_home}/.config;XDG_DATA_HOME=${omabook_test_home}/.local/share;XDG_CACHE_HOME=${omabook_test_home}/.cache")
# Names sort the same under a C locale as under a language's (#121): the
# suites that depend on it run again with this (tests/<name>_tests.cmake).
set(omabook_c_locale_environment "${omabook_test_environment};LANG=C.UTF-8;LC_ALL=C.UTF-8")
include(tests/suites.cmake)
omabook_test_suites(omabook_suites "${CMAKE_SOURCE_DIR}/tests")
foreach(suite IN LISTS omabook_suites)
  add_executable(${suite} tests/${suite}.cpp tests/fixtures.cpp src/resources.qrc ${omabook_themes})
  target_link_libraries(${suite} PRIVATE omabook_core Qt6::Test)
  add_test(NAME ${suite} COMMAND ${suite})
  set_tests_properties(${suite} PROPERTIES ENVIRONMENT "${omabook_test_environment}")
  if(EXISTS "${CMAKE_SOURCE_DIR}/tests/${suite}.cmake")
    include("${CMAKE_SOURCE_DIR}/tests/${suite}.cmake")
  endif()
endforeach()

# The commands agents use, driven through the real binary with no display.
add_test(NAME cli_tests COMMAND ${CMAKE_SOURCE_DIR}/tests/cli_tests.sh $<TARGET_FILE:omabook>)
set_tests_properties(cli_tests PROPERTIES ENVIRONMENT "${omabook_test_environment}")
# The tools that keep tickets from meeting on shared files (#198): the change
# log's fragments, the merge drivers, the suites found by name.
add_test(NAME workflow_tests COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/workflow_tests.py)
set_tests_properties(workflow_tests PROPERTIES ENVIRONMENT "${omabook_test_environment};CMAKE_COMMAND=${CMAKE_COMMAND}")

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS omabook RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/omabook.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/omabook.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/omabook)
# Omabook's plugin for Folio, the file manager: each library in the sidebar
# as a folder of its books, and its entries on book files.
install(DIRECTORY share/folio/ DESTINATION ${CMAKE_INSTALL_DATADIR}/folio/plugins)
# .mdz, a zipped Markdown book, as a type of its own.
install(FILES share/omabook-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages RENAME omabook.xml)
install(FILES share/omabook/calibre_bridge.py DESTINATION ${CMAKE_INSTALL_DATADIR}/omabook)
