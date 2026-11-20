cmake_minimum_required(VERSION 3.24)
project(omabook VERSION 0.1.8 LANGUAGES CXX)

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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Sql Concurrent Test)
find_package(PkgConfig REQUIRED)
# libarchive opens EPUB, CBZ, CBR and friends; Poppler renders PDF; libsodium
# keeps a person's key pair and seals what a shared space sends (ADR 0001).
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(LIBSODIUM REQUIRED IMPORTED_TARGET libsodium)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB omabook_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM omabook_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(omabook_core STATIC ${omabook_sources})
target_include_directories(omabook_core PUBLIC src)
target_link_libraries(omabook_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus Qt6::Sql Qt6::Concurrent
  PkgConfig::LIBARCHIVE PkgConfig::POPPLER_QT6 PkgConfig::LIBSODIUM)
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
foreach(test formats library services graphsync spaces replication ui manual mcp calibre folio readingsites scripting pluginmanager pluginnaming graphview graph reading layout identity sharing pairing subscribing sharedgraph sharedspace linking sharingwindow opening upgrading pluginsync pluginformats upgradedsync duplicates restoring touch foundby graphcut chronology aiservers pluginworlds annotationsync chronicle chronologybuild spacefiles chronologycli chronologyview chronologywindow largelog tenancy bookview tenantswitch firstopen tenantreaders graphdegree tenantwrites graphsearch whatsnew worldcli)
  add_executable(${test}_tests tests/${test}_tests.cpp tests/fixtures.cpp src/resources.qrc ${omabook_themes})
  target_link_libraries(${test}_tests PRIVATE omabook_core Qt6::Test)
  add_test(NAME ${test}_tests COMMAND ${test}_tests)
  set_tests_properties(${test}_tests PROPERTIES ENVIRONMENT "${omabook_test_environment}")
endforeach()

# Syncing runs the other computer's omabook over ssh; the test's "other
# computer" is this build's binary behind a stand-in ssh.
add_dependencies(services_tests omabook)
target_compile_definitions(services_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
add_dependencies(aiservers_tests omabook)
target_compile_definitions(aiservers_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
add_dependencies(graphsync_tests omabook)
target_compile_definitions(graphsync_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
add_dependencies(spaces_tests omabook)
target_compile_definitions(spaces_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
add_dependencies(sharing_tests omabook)
target_compile_definitions(sharing_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# The library samples of every release, for the tests of the change log and of syncing through it.
target_sources(spaces_tests PRIVATE tests/samples.cpp)
target_sources(replication_tests PRIVATE tests/samples.cpp)
target_sources(upgrading_tests PRIVATE tests/samples.cpp)
target_sources(tenancy_tests PRIVATE tests/samples.cpp)
target_sources(graphsearch_tests PRIVATE tests/samples.cpp)
target_sources(firstopen_tests PRIVATE tests/samples.cpp)
# The chronology's command and tools, run as a person and an agent meet them (#45).
add_dependencies(chronologycli_tests omabook)
target_compile_definitions(chronologycli_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
add_dependencies(worldcli_tests omabook)
target_compile_definitions(worldcli_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# The first start after an update, with a plugin installed, runs the real binary (#60).
add_dependencies(upgrading_tests omabook)
target_compile_definitions(upgrading_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
add_dependencies(replication_tests omabook)
target_compile_definitions(replication_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# Two libraries brought up from 0.1.5 and synced over the stand-in ssh.
target_sources(upgradedsync_tests PRIVATE tests/samples.cpp)
add_dependencies(upgradedsync_tests omabook)
target_compile_definitions(upgradedsync_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# Two copies of a book on one computer and one on the other, from the 0.1.5 sample, synced over the stand-in ssh (#58).
target_sources(duplicates_tests PRIVATE tests/samples.cpp)
add_dependencies(duplicates_tests omabook)
target_compile_definitions(duplicates_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# Bookmarks and highlights between two libraries over the stand-in ssh, of a sample too (#66).
target_sources(annotationsync_tests PRIVATE tests/samples.cpp)
add_dependencies(annotationsync_tests omabook)
target_compile_definitions(annotationsync_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# A library put back from a backup, or written by another program, then synced over the stand-in ssh (#61).
add_dependencies(restoring_tests omabook)
target_compile_definitions(restoring_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# Pairing two computers syncs with the other one's omabook, over the stand-in ssh.
add_dependencies(pairing_tests omabook)
target_compile_definitions(pairing_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# A book in a plugin's format synced to another computer, over the stand-in ssh.
add_dependencies(pluginsync_tests omabook)
target_compile_definitions(pluginsync_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# A large change log synced, and the log a pairing brings in compacted, over the stand-in ssh (#103).
target_sources(largelog_tests PRIVATE tests/samples.cpp)
add_dependencies(largelog_tests omabook)
target_compile_definitions(largelog_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# A book in the format of a plugin that isn't installed, synced over the stand-in ssh.
add_dependencies(pluginformats_tests omabook)
target_compile_definitions(pluginformats_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")

# The Folio plugins are read from the source tree, as they are installed.
target_compile_definitions(folio_tests PRIVATE OMABOOK_SOURCE="${CMAKE_SOURCE_DIR}")
target_compile_definitions(folio_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
target_compile_definitions(folio_tests PRIVATE OMABOOK_TEST_TMP="${CMAKE_BINARY_DIR}")
add_dependencies(folio_tests omabook)

# The MCP server is tested the way an agent meets it: the real binary, spoken
# to over standard input and output.
add_dependencies(mcp_tests omabook)
target_compile_definitions(mcp_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")

# The knowledge graph's commands and tools are tested through the real binary too.
add_dependencies(graph_tests omabook)
target_compile_definitions(graph_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# Two commands open one new library at once (#11).
add_dependencies(opening_tests omabook)
target_compile_definitions(opening_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
add_dependencies(subscribing_tests omabook)
target_compile_definitions(subscribing_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
add_dependencies(sharedgraph_tests omabook)
target_compile_definitions(sharedgraph_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# A time, an order and a timeline travel to another computer through the log (#41).
add_dependencies(chronology_tests omabook)
target_compile_definitions(chronology_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>")
# Sharing by link sends gists through GitHub's command line; the tests' gh is
# a stand-in (tests/fakegh.cpp) that sends to the fake GitHub, in a folder of
# its own so that a test can put it first on PATH.
add_executable(fakegh tests/fakegh.cpp)
target_link_libraries(fakegh PRIVATE Qt6::Core Qt6::Network)
set_target_properties(fakegh PROPERTIES OUTPUT_NAME gh RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake-gh")
add_dependencies(linking_tests omabook fakegh)
target_compile_definitions(linking_tests PRIVATE OMABOOK_BINARY="$<TARGET_FILE:omabook>" FAKE_GH="$<TARGET_FILE:fakegh>")

# The commands agents use, driven through the real binary with no display.
add_test(NAME cli_tests COMMAND ${CMAKE_SOURCE_DIR}/tests/cli_tests.sh $<TARGET_FILE:omabook>)
set_tests_properties(cli_tests PROPERTIES ENVIRONMENT "${omabook_test_environment}")

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
