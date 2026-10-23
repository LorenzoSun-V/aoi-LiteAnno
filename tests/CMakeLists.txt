option(LITEANNO_SKIP_PYTHON_TESTS
    "Do not run the byte-format comparison with Python's json module (needs Python 3)" OFF)
set(LITEANNO_UPSTREAM_PYTHON "" CACHE FILEPATH
    "A Python with X-AnyLabeling (x-anylabeling-cvhub) 4.2.0 installed; runs the round trips of format spec 11")

set(LITEANNO_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-results")

# Tests find the Qt DLLs next to Qt6Core.dll on Windows.
function(liteanno_set_test_environment test)
    if(WIN32)
        set_property(TEST ${test} PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endfunction()

# A separate process for tests that kill a save midway or leave a lock behind.
add_executable(liteanno_test_helper tools/test_helper.cpp common/ReliableSaveTestData.h)
target_include_directories(liteanno_test_helper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(liteanno_test_helper PRIVATE liteanno_application Qt6::Core)

function(liteanno_add_test name)
    add_executable(${name} ${ARGN} common/TestData.h common/FileBlocker.h common/FolderBlocker.h)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE
        liteanno_application liteanno_platform liteanno_formats liteanno_core liteanno_nlohmann_json Qt6::Test)
    target_compile_definitions(${name} PRIVATE
        LITEANNO_TEST_DATA_DIR="${LITEANNO_TEST_DATA_DIR}"
        LITEANNO_TEST_HELPER="$<TARGET_FILE:liteanno_test_helper>")
    # Each item's result also goes to a file, so CI can list skipped items on every platform.
    add_test(NAME ${name} COMMAND ${name} -o -,txt -o "${CMAKE_BINARY_DIR}/test-results/${name}.txt,txt")
    liteanno_set_test_environment(${name})
endfunction()

liteanno_add_test(tst_annotationdocument core/tst_annotationdocument.cpp)
liteanno_add_test(tst_validation core/tst_validation.cpp)
liteanno_add_test(tst_imagepath core/tst_imagepath.cpp)
liteanno_add_test(tst_geometry core/tst_geometry.cpp)
liteanno_add_test(tst_bundledlicenses core/tst_bundledlicenses.cpp)
file(GLOB _test_licenses "${LITEANNO_TEST_DATA_DIR}/bundled-licenses/*")
qt_add_resources(tst_bundledlicenses "test_licenses" PREFIX "/licenses" BASE "${LITEANNO_TEST_DATA_DIR}/bundled-licenses"
    FILES ${_test_licenses})
liteanno_add_test(tst_jsontext formats/tst_jsontext.cpp)
liteanno_add_test(tst_annotationread formats/tst_annotationread.cpp)
liteanno_add_test(tst_annotationwrite formats/tst_annotationwrite.cpp)
liteanno_add_test(tst_annotationstore formats/tst_annotationstore.cpp)
liteanno_add_test(tst_worksetfiles formats/tst_worksetfiles.cpp)
liteanno_add_test(tst_projectfile formats/tst_projectfile.cpp)
liteanno_add_test(tst_recoveryrecord formats/tst_recoveryrecord.cpp)
liteanno_add_test(tst_volumebinding platform/tst_volumebinding.cpp)
liteanno_add_test(tst_reliablesave application/tst_reliablesave.cpp common/ReliableSaveTestData.h)
add_dependencies(tst_reliablesave liteanno_test_helper)
liteanno_add_test(tst_documentsession application/tst_documentsession.cpp common/ReliableSaveTestData.h)
liteanno_add_test(tst_directoryscanner dataset/tst_directoryscanner.cpp)
liteanno_add_test(tst_catalog dataset/tst_catalog.cpp)
liteanno_add_test(tst_projectcatalog application/tst_projectcatalog.cpp)
liteanno_add_test(tst_worksetstore application/tst_worksetstore.cpp)

# Architecture section 12: the pin check rejects any other Qt patch or MSVC compiler build.
add_test(NAME toolchain_pin_rejects_other_versions
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain_pin_test.cmake")

# Format spec 13.5: the bytes this program writes must equal Python's
# json.dumps(value, ensure_ascii=False, indent=2) for the same value.
add_executable(liteanno_json_reformat tools/json_reformat.cpp)
target_link_libraries(liteanno_json_reformat PRIVATE liteanno_formats liteanno_nlohmann_json Qt6::Core)

if(NOT LITEANNO_SKIP_PYTHON_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME byte_format_matches_python
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/python/check_byte_format.py"
                "$<TARGET_FILE:liteanno_json_reformat>"
                "${LITEANNO_TEST_DATA_DIR}/annotations"
                "${CMAKE_CURRENT_BINARY_DIR}/byte_format")
    liteanno_set_test_environment(byte_format_matches_python)
endif()

if(LITEANNO_BUILD_UI)
    add_test(NAME ui_starts COMMAND liteanno_ui --smoke-test)
    set_property(TEST ui_starts PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    liteanno_set_test_environment(ui_starts)

    liteanno_add_test(tst_imageloader imaging/tst_imageloader.cpp)
    target_link_libraries(tst_imageloader PRIVATE liteanno_imaging)
    set_property(TEST tst_imageloader PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    liteanno_add_test(tst_export application/tst_export.cpp)
    target_link_libraries(tst_export PRIVATE liteanno_imaging)
    target_compile_definitions(tst_export PRIVATE LITEANNO_CLI="$<TARGET_FILE:liteanno_cli>")
    add_dependencies(tst_export liteanno_cli)

    liteanno_add_test(tst_import application/tst_import.cpp)
    target_link_libraries(tst_import PRIVATE liteanno_imaging)

    # Format spec 11: round trips checked with X-AnyLabeling's own reader and writer.
    add_executable(liteanno_compat_roundtrip tools/compat_roundtrip.cpp)
    target_link_libraries(liteanno_compat_roundtrip PRIVATE liteanno_imaging liteanno_formats Qt6::Gui)
    if(LITEANNO_UPSTREAM_PYTHON)
        add_test(NAME upstream_compat
            COMMAND "${LITEANNO_UPSTREAM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/python/upstream_compat.py"
                    "$<TARGET_FILE:liteanno_compat_roundtrip>"
                    "${LITEANNO_TEST_DATA_DIR}/annotations"
                    "${CMAKE_CURRENT_BINARY_DIR}/upstream_compat")
        set_property(TEST upstream_compat PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        liteanno_set_test_environment(upstream_compat)
        # The checks must catch a writer that turns original true/false values into 1/0.
        add_test(NAME upstream_compat_catches_changed_types
            COMMAND "${LITEANNO_UPSTREAM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/python/upstream_compat.py"
                    "$<TARGET_FILE:liteanno_compat_roundtrip>"
                    "${LITEANNO_TEST_DATA_DIR}/annotations"
                    "${CMAKE_CURRENT_BINARY_DIR}/upstream_compat_tamper" --tamper)
        set_property(TEST upstream_compat_catches_changed_types PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        liteanno_set_test_environment(upstream_compat_catches_changed_types)
        # Architecture 11.1: each export format against upstream's own converter.
        add_test(NAME upstream_export
            COMMAND "${LITEANNO_UPSTREAM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/python/export_compat.py"
                    "$<TARGET_FILE:liteanno_cli>" "${CMAKE_CURRENT_BINARY_DIR}/upstream_export")
        set_property(TEST upstream_export PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        liteanno_set_test_environment(upstream_export)
        # Architecture 11.2: each import format against upstream's own converter.
        add_test(NAME upstream_import
            COMMAND "${LITEANNO_UPSTREAM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/python/import_compat.py"
                    "$<TARGET_FILE:liteanno_cli>" "${CMAKE_CURRENT_BINARY_DIR}/upstream_import")
        set_property(TEST upstream_import PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        liteanno_set_test_environment(upstream_import)
    endif()

    # Architecture 14: catalog timings; CI runs it with larger folders and prints the table.
    add_executable(liteanno_catalog_benchmark tools/catalog_benchmark.cpp)
    target_link_libraries(liteanno_catalog_benchmark PRIVATE liteanno_ui_lib liteanno_application liteanno_imaging Qt6::Widgets)
    if(WIN32)
        target_link_libraries(liteanno_catalog_benchmark PRIVATE psapi)
    endif()
    add_test(NAME catalog_benchmark_runs
        COMMAND liteanno_catalog_benchmark "${CMAKE_CURRENT_BINARY_DIR}/benchmark" 2000)
    liteanno_set_test_environment(catalog_benchmark_runs)

    liteanno_add_test(tst_mainwindow ui/tst_mainwindow.cpp)
    target_link_libraries(tst_mainwindow PRIVATE liteanno_ui_lib)
    set_property(TEST tst_mainwindow PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
