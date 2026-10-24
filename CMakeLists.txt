# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILES_VERSION)
string(STRIP "${FILES_VERSION}" FILES_VERSION)
project(Files VERSION "${FILES_VERSION}" LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(INFILTRATOR_BUILD_PROFILE "release" CACHE STRING "Files build profile recorded in BUILD-INFO")
set(INFILTRATOR_PGO_STATUS "not trained" CACHE STRING "Files PGO status recorded in BUILD-INFO")

set(INFILTRATR_COMMON_MINIMUM_VERSION "1.19.66")
set(INFILTRATR_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/infiltratr-common")

if(NOT EXISTS "${INFILTRATR_COMMON_DIR}/VERSION")
    message(FATAL_ERROR
        "Files requires the pinned Common submodule. "
        "Clone with --recurse-submodules or run git submodule update --init --recursive.")
endif()

file(READ "${INFILTRATR_COMMON_DIR}/VERSION" INFILTRATR_COMMON_VERSION)
string(STRIP "${INFILTRATR_COMMON_VERSION}" INFILTRATR_COMMON_VERSION)
if(INFILTRATR_COMMON_VERSION VERSION_LESS INFILTRATR_COMMON_MINIMUM_VERSION)
    message(FATAL_ERROR
        "Files requires Common ${INFILTRATR_COMMON_MINIMUM_VERSION} or newer; "
        "found ${INFILTRATR_COMMON_VERSION}")
endif()

# The gitlink is the single authority for the exact Common commit. Git is only
# a build dependency for a live checkout; exported source can qualify the
# compatible dependency through Common's minimum API version without a .git database.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                ls-tree HEAD -- src/infiltratr-common
        RESULT_VARIABLE INFILTRATR_COMMON_GITLINK_RESULT
        OUTPUT_VARIABLE INFILTRATR_COMMON_GITLINK
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    # CMake's regular-expression dialect does not implement {N} repetition.
    # Parse the complete gitlink record, then validate the extracted SHA length.
    string(REGEX MATCH "160000[ \t]+commit[ \t]+[0-9a-fA-F]+"
           INFILTRATR_COMMON_GITLINK_RECORD "${INFILTRATR_COMMON_GITLINK}")
    string(REGEX REPLACE "^160000[ \t]+commit[ \t]+" ""
           INFILTRATR_COMMON_EXPECTED_COMMIT "${INFILTRATR_COMMON_GITLINK_RECORD}")
    string(LENGTH "${INFILTRATR_COMMON_EXPECTED_COMMIT}"
           INFILTRATR_COMMON_EXPECTED_COMMIT_LENGTH)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${INFILTRATR_COMMON_DIR}" rev-parse HEAD
        RESULT_VARIABLE INFILTRATR_COMMON_GIT_RESULT
        OUTPUT_VARIABLE INFILTRATR_COMMON_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(NOT INFILTRATR_COMMON_GITLINK_RESULT EQUAL 0 OR
       NOT INFILTRATR_COMMON_EXPECTED_COMMIT_LENGTH EQUAL 40 OR
       NOT INFILTRATR_COMMON_GIT_RESULT EQUAL 0 OR
       NOT INFILTRATR_COMMON_COMMIT STREQUAL INFILTRATR_COMMON_EXPECTED_COMMIT)
        message(FATAL_ERROR
            "Files requires the Common commit recorded by its gitlink "
            "(${INFILTRATR_COMMON_EXPECTED_COMMIT}); found ${INFILTRATR_COMMON_COMMIT}")
    endif()
endif()

set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
add_subdirectory("${INFILTRATR_COMMON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/infiltratr-common" EXCLUDE_FROM_ALL)

pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.10)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GDKPIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)

function(infiltrator_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    endif()
endfunction()

add_library(infiltrator-files-core STATIC
    src/core/batch_removal_engine.cpp
    src/core/batch_transfer_engine.cpp
    src/core/copied_metadata.cpp
    src/core/location.cpp
    src/core/mounted_places.cpp
    src/core/operation_engine.cpp
    src/core/operation_journal.cpp
    src/core/recovery_operation_engine.cpp
    src/core/search_engine.cpp
    src/core/transfer_operation_engine.cpp)
target_include_directories(infiltrator-files-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
target_compile_features(infiltrator-files-core PUBLIC cxx_std_20)
target_link_libraries(infiltrator-files-core PUBLIC
    InfiltratrCommon::Common
    PkgConfig::GIO)
infiltrator_enable_warnings(infiltrator-files-core)

# Compile the GTK implementation once. The application and hosted UI tests
# consume this target instead of maintaining duplicate source-file inventories.
add_library(infiltrator-files-ui STATIC
    src/ui/batch_item_operation_controller.cpp
    src/ui/clipboard_controller.cpp
    src/ui/create_folder_controller.cpp
    src/ui/destructive_operation_controller.cpp
    src/ui/detail_metadata.cpp
    src/ui/directory_mutation_guard.cpp
    src/ui/drag_drop_controller.cpp
    src/ui/file_manager_window.cpp
    src/ui/file_inspection.cpp
    src/ui/interaction_controller.cpp
    src/ui/item_operation_controller.cpp
    src/ui/journal_recovery_controller.cpp
    src/ui/location_open.cpp
    src/ui/operation_surface_install.cpp
    src/ui/presentation_store.cpp
    src/ui/quick_preview.cpp
    src/ui/recursive_search.cpp
    src/ui/search_persistence.cpp
    src/ui/tag_catalogue.cpp
    src/ui/theme_controller.cpp
    src/ui/transfer_progress_view.cpp
    src/ui/workspace_tabs.cpp
    src/ui/workspace_session.cpp
    src/ui/workspace_bookmarks.cpp
    src/ui/workspace_folder_views.cpp
    src/ui/thumbnail_cache.cpp)
target_include_directories(infiltrator-files-ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(infiltrator-files-ui PUBLIC cxx_std_20)
target_link_libraries(infiltrator-files-ui PUBLIC
    infiltrator-files-core
    PkgConfig::GTK4
    PkgConfig::GDKPIXBUF)
infiltrator_enable_warnings(infiltrator-files-ui)

add_executable(files src/main.cpp)
target_link_libraries(files PRIVATE infiltrator-files-ui)
infiltrator_enable_warnings(files)

function(infiltrator_add_core_test target source test_name)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE infiltrator-files-core)
    infiltrator_enable_warnings(${target})
    add_test(NAME ${test_name} COMMAND ${target})
endfunction()

if(BUILD_TESTING)
    infiltrator_add_core_test(location-model-test tests/location_model_test.cpp location-model)

    add_library(calendar-date-test-fixture MODULE tests/calendar_date_fixture.c)
    target_link_libraries(calendar-date-test-fixture PRIVATE InfiltratrCommon::Portable)

    add_executable(tag-catalogue-test tests/tag_catalogue_test.cpp)
    target_link_libraries(tag-catalogue-test PRIVATE infiltrator-files-ui)
    infiltrator_enable_warnings(tag-catalogue-test)
    add_test(NAME tag-catalogue COMMAND tag-catalogue-test)

    add_executable(detail-metadata-test tests/detail_metadata_test.cpp)
    target_compile_definitions(detail-metadata-test PRIVATE
        CALENDAR_DATE_FIXTURE_PATH="$<TARGET_FILE:calendar-date-test-fixture>")
    add_dependencies(detail-metadata-test calendar-date-test-fixture)
    target_link_libraries(detail-metadata-test PRIVATE infiltrator-files-ui)
    infiltrator_enable_warnings(detail-metadata-test)
    add_test(NAME detail-metadata COMMAND detail-metadata-test)

    infiltrator_add_core_test(mounted-places-test tests/mounted_places_test.cpp mounted-places)
    infiltrator_add_core_test(search-engine-test tests/search_engine_test.cpp search-engine)

    add_executable(window-state-test tests/window_state_test.cpp)
    target_link_libraries(window-state-test PRIVATE infiltrator-files-ui)
    infiltrator_enable_warnings(window-state-test)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
    if(XVFB_RUN_EXECUTABLE AND DBUS_RUN_SESSION_EXECUTABLE)
        add_test(NAME window-state COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
            "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:window-state-test>)
        set_tests_properties(window-state PROPERTIES
            ENVIRONMENT "G_DEBUG=fatal-criticals;GTK_A11Y=none;GSETTINGS_BACKEND=memory"
            TIMEOUT 45)

        add_executable(operation-lifetime-test tests/operation_lifetime_test.cpp)
        target_link_libraries(operation-lifetime-test PRIVATE infiltrator-files-ui)
        infiltrator_enable_warnings(operation-lifetime-test)
        target_link_options(operation-lifetime-test PRIVATE "-Wl,--wrap=fsync")
        add_test(NAME operation-lifetime COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
            "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:operation-lifetime-test>)
        set_tests_properties(operation-lifetime PROPERTIES
            ENVIRONMENT "G_DEBUG=fatal-criticals;GTK_A11Y=none;GSETTINGS_BACKEND=memory"
            TIMEOUT 45)

        add_executable(workspace-navigation-test tests/workspace_navigation_test.cpp)
        target_link_libraries(workspace-navigation-test PRIVATE infiltrator-files-ui)
        infiltrator_enable_warnings(workspace-navigation-test)
        add_test(NAME workspace-navigation COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
            "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:workspace-navigation-test>)
        set_tests_properties(workspace-navigation PROPERTIES
            ENVIRONMENT "G_DEBUG=fatal-criticals;GTK_A11Y=none;GSETTINGS_BACKEND=memory"
            TIMEOUT 45)

        add_executable(workspace-features-test tests/workspace_features_test.cpp)
        target_link_libraries(workspace-features-test PRIVATE infiltrator-files-ui)
        infiltrator_enable_warnings(workspace-features-test)
        add_test(NAME workspace-features COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
            "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:workspace-features-test>)
        set_tests_properties(workspace-features PROPERTIES
            ENVIRONMENT "G_DEBUG=fatal-criticals;GTK_A11Y=none;GSETTINGS_BACKEND=memory"
            TIMEOUT 45)

        # Theme integration checks the real GTK colour cascade under Xvfb,
        # including per-window Day/Night isolation and root startup.
        add_executable(theme-presentation-test tests/theme_presentation_test.cpp)
        target_link_libraries(theme-presentation-test PRIVATE infiltrator-files-ui)
        infiltrator_enable_warnings(theme-presentation-test)
        add_test(NAME theme-presentation COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
            "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:theme-presentation-test>)
        set_tests_properties(theme-presentation PROPERTIES
            ENVIRONMENT "G_DEBUG=fatal-criticals;GTK_A11Y=none;GSETTINGS_BACKEND=memory"
            TIMEOUT 45)

        add_executable(interaction-surface-test tests/interaction_surface_test.cpp)
        target_link_libraries(interaction-surface-test PRIVATE infiltrator-files-ui)
        infiltrator_enable_warnings(interaction-surface-test)
        add_test(NAME interaction-surface COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
            "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:interaction-surface-test>)
        set_tests_properties(interaction-surface PROPERTIES
            ENVIRONMENT "G_DEBUG=fatal-criticals;GTK_A11Y=none;GSETTINGS_BACKEND=memory"
            TIMEOUT 45)
    else()
        message(STATUS "Headless window-state/interaction-surface tests require xvfb-run and dbus-run-session")
    endif()

    infiltrator_add_core_test(operation-engine-test tests/operation_engine_test.cpp operation-engine)
    infiltrator_add_core_test(destination-ownership-test tests/destination_ownership_test.cpp destination-ownership)
    infiltrator_add_core_test(recovery-operation-engine-test tests/recovery_operation_engine_test.cpp recovery-operation-engine)
    infiltrator_add_core_test(transfer-operation-engine-test tests/transfer_operation_engine_test.cpp transfer-operation-engine)
    infiltrator_add_core_test(batch-transfer-engine-test tests/batch_transfer_engine_test.cpp batch-transfer-engine)
    infiltrator_add_core_test(batch-removal-engine-test tests/batch_removal_engine_test.cpp batch-removal-engine)
    infiltrator_add_core_test(operation-journal-test tests/operation_journal_test.cpp operation-journal)
    infiltrator_add_core_test(transfer-safety-test tests/transfer_safety_test.cpp transfer-safety)
    target_link_options(transfer-safety-test PRIVATE "-Wl,--wrap=fsync" "-Wl,--wrap=fsetxattr")
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/BUILD-INFO"
    "Profile: ${INFILTRATOR_BUILD_PROFILE}\n"
    "PGO: ${INFILTRATOR_PGO_STATUS}\n"
    "C flags: ${CMAKE_C_FLAGS_RELEASE}\n"
    "CXX flags: ${CMAKE_CXX_FLAGS_RELEASE}\n")

# Native filesystem contract: launchable Infiltrator applications live directly
# under /Programs. Desktop integration remains in the host's standard metadata
# locations and points at the canonical executable explicitly, so a standalone
# Files install works even when /Programs is not yet in the shell PATH.
install(TARGETS files
    RUNTIME DESTINATION "/Programs")
install(FILES data/files.desktop
    DESTINATION "/usr/share/applications")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/BUILD-INFO"
    DESTINATION "/usr/share/doc/files")

# Standalone Files must resolve the same verified 23-face family as a full
# InfiltratorOS installation. Font binaries belong to the persistent machine
# resource cache, not this package. The helper comes from pinned Common.
install(PROGRAMS
    "${INFILTRATR_COMMON_DIR}/cmake/InfiltratrEnsureExtendedFonts.sh"
    DESTINATION "/usr/lib/files"
    RENAME "ensure-extended-fonts")

set(CPACK_PACKAGE_NAME "files")
set(CPACK_PACKAGE_VENDOR "Infiltrator Projects")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "InfiltratorOS file manager")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Infiltrator Projects")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# One release requirement also drives native-installer CI dependency setup.
set(INFILTRATOR_TEMPORAL_RESOURCES_VERSION "1.19.66")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/temporal-resources-version"
    CONTENT "${INFILTRATOR_TEMPORAL_RESOURCES_VERSION}\n")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "ca-certificates, fontconfig, curl | wget")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "infiltrator-calendar-resources (>= ${INFILTRATOR_TEMPORAL_RESOURCES_VERSION})")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
include(CPack)
