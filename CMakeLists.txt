# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" INFILTRATOR_FILE_MANAGER_VERSION)
string(STRIP "${INFILTRATOR_FILE_MANAGER_VERSION}" INFILTRATOR_FILE_MANAGER_VERSION)
project(InfiltratorFileManager VERSION "${INFILTRATOR_FILE_MANAGER_VERSION}" LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Git REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(INFILTRATR_COMMON_EXPECTED_VERSION "1.19.41")
set(INFILTRATR_COMMON_EXPECTED_COMMIT "b0387b7ab5d71cda60748e2506b85cdc0e6b5e8b")
set(INFILTRATR_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/infiltratr-common")

if(NOT EXISTS "${INFILTRATR_COMMON_DIR}/VERSION")
    message(FATAL_ERROR
        "Infiltrator File Manager requires the pinned Common submodule. "
        "Clone with --recurse-submodules or run git submodule update --init --recursive.")
endif()

file(READ "${INFILTRATR_COMMON_DIR}/VERSION" INFILTRATR_COMMON_VERSION)
string(STRIP "${INFILTRATR_COMMON_VERSION}" INFILTRATR_COMMON_VERSION)
if(NOT INFILTRATR_COMMON_VERSION STREQUAL INFILTRATR_COMMON_EXPECTED_VERSION)
    message(FATAL_ERROR
        "Infiltrator File Manager requires Common ${INFILTRATR_COMMON_EXPECTED_VERSION}; "
        "found ${INFILTRATR_COMMON_VERSION}")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${INFILTRATR_COMMON_DIR}" rev-parse HEAD
        RESULT_VARIABLE INFILTRATR_COMMON_GIT_RESULT
        OUTPUT_VARIABLE INFILTRATR_COMMON_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT INFILTRATR_COMMON_GIT_RESULT EQUAL 0 OR
       NOT INFILTRATR_COMMON_COMMIT STREQUAL INFILTRATR_COMMON_EXPECTED_COMMIT)
        message(FATAL_ERROR
            "Infiltrator File Manager requires Common commit "
            "${INFILTRATR_COMMON_EXPECTED_COMMIT}; found ${INFILTRATR_COMMON_COMMIT}")
    endif()
endif()

set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
add_subdirectory("${INFILTRATR_COMMON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/infiltratr-common" EXCLUDE_FROM_ALL)

pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.10)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

add_library(infiltrator-files-core STATIC
    src/core/batch_transfer_engine.cpp
    src/core/location.cpp
    src/core/mounted_places.cpp
    src/core/operation_engine.cpp
    src/core/operation_journal.cpp
    src/core/recovery_operation_engine.cpp
    src/core/transfer_operation_engine.cpp)
target_include_directories(infiltrator-files-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
target_compile_features(infiltrator-files-core PUBLIC cxx_std_20)
target_link_libraries(infiltrator-files-core PUBLIC
    InfiltratrCommon::Common
    PkgConfig::GIO)

add_executable(infiltrator-file-manager
    src/main.cpp
    src/ui/batch_item_operation_controller.cpp
    src/ui/create_folder_controller.cpp
    src/ui/destructive_operation_controller.cpp
    src/ui/detail_metadata.cpp
    src/ui/file_manager_window.cpp
    src/ui/item_operation_controller.cpp
    src/ui/journal_recovery_controller.cpp
    src/ui/theme_controller.cpp)
target_include_directories(infiltrator-file-manager PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(infiltrator-file-manager PRIVATE
    infiltrator-files-core
    PkgConfig::GTK4)

foreach(target infiltrator-files-core infiltrator-file-manager)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    endif()
endforeach()

if(BUILD_TESTING)
    add_executable(location-model-test tests/location_model_test.cpp)
    target_link_libraries(location-model-test PRIVATE infiltrator-files-core)
    add_test(NAME location-model COMMAND location-model-test)

    add_executable(detail-metadata-test
        tests/detail_metadata_test.cpp
        src/ui/detail_metadata.cpp)
    add_library(calendar-date-test-fixture MODULE tests/calendar_date_fixture.c)
    target_compile_definitions(detail-metadata-test PRIVATE
        CALENDAR_DATE_FIXTURE_PATH="$<TARGET_FILE:calendar-date-test-fixture>")
    add_dependencies(detail-metadata-test calendar-date-test-fixture)
    target_link_libraries(detail-metadata-test PRIVATE infiltrator-files-core)
    add_test(NAME detail-metadata COMMAND detail-metadata-test)

    add_executable(mounted-places-test tests/mounted_places_test.cpp)
    target_link_libraries(mounted-places-test PRIVATE infiltrator-files-core)
    add_test(NAME mounted-places COMMAND mounted-places-test)

    add_executable(window-state-test
        tests/window_state_test.cpp
        src/ui/file_manager_window.cpp
        src/ui/detail_metadata.cpp)
    target_link_libraries(window-state-test PRIVATE infiltrator-files-core PkgConfig::GTK4)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
    if(XVFB_RUN_EXECUTABLE AND DBUS_RUN_SESSION_EXECUTABLE)
        add_test(NAME window-state COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
            "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:window-state-test>)
        set_tests_properties(window-state PROPERTIES
            ENVIRONMENT "G_DEBUG=fatal-criticals;GTK_A11Y=none;GSETTINGS_BACKEND=memory"
            TIMEOUT 45)
    else()
        message(STATUS "Headless window-state test requires xvfb-run and dbus-run-session")
    endif()

    add_executable(operation-engine-test tests/operation_engine_test.cpp)
    target_link_libraries(operation-engine-test PRIVATE infiltrator-files-core)
    add_test(NAME operation-engine COMMAND operation-engine-test)

    add_executable(destination-ownership-test tests/destination_ownership_test.cpp)
    target_link_libraries(destination-ownership-test PRIVATE infiltrator-files-core)
    add_test(NAME destination-ownership COMMAND destination-ownership-test)

    add_executable(recovery-operation-engine-test tests/recovery_operation_engine_test.cpp)
    target_link_libraries(recovery-operation-engine-test PRIVATE infiltrator-files-core)
    add_test(NAME recovery-operation-engine COMMAND recovery-operation-engine-test)

    add_executable(transfer-operation-engine-test tests/transfer_operation_engine_test.cpp)
    target_link_libraries(transfer-operation-engine-test PRIVATE infiltrator-files-core)
    add_test(NAME transfer-operation-engine COMMAND transfer-operation-engine-test)

    add_executable(batch-transfer-engine-test tests/batch_transfer_engine_test.cpp)
    target_link_libraries(batch-transfer-engine-test PRIVATE infiltrator-files-core)
    add_test(NAME batch-transfer-engine COMMAND batch-transfer-engine-test)

    add_executable(operation-journal-test tests/operation_journal_test.cpp)
    target_link_libraries(operation-journal-test PRIVATE infiltrator-files-core)
    add_test(NAME operation-journal COMMAND operation-journal-test)
endif()

install(TARGETS infiltrator-file-manager
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES data/infiltrator-file-manager.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

set(CPACK_PACKAGE_NAME "infiltrator-file-manager")
set(CPACK_PACKAGE_VENDOR "Infiltrator Projects")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "InfiltratorOS file manager")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Infiltrator Projects")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
