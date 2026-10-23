# Files the single-file Windows build (static Qt, .github/workflows/package.yml) carries
# inside, since nothing is next to it: the licence texts of what is linked in, for both
# programs (core/BundledLicenses.h), and Qt's Chinese dialog texts for the window.
set(LITEANNO_QT_TRANSLATION "" CACHE FILEPATH "qtbase_zh_CN.qm to build into the window program")
set(LITEANNO_BUNDLED_LICENSES_DIR "" CACHE PATH "Folder of licence texts to build into both programs")

function(liteanno_bundle_licenses target)
    if(NOT LITEANNO_BUNDLED_LICENSES_DIR)
        return()
    endif()
    file(GLOB_RECURSE _licenses LIST_DIRECTORIES false "${LITEANNO_BUNDLED_LICENSES_DIR}/*")
    if(NOT _licenses)
        message(FATAL_ERROR "No licence texts in ${LITEANNO_BUNDLED_LICENSES_DIR}")
    endif()
    qt_add_resources(${target} "${target}_licenses" PREFIX "/licenses" BASE "${LITEANNO_BUNDLED_LICENSES_DIR}"
        FILES ${_licenses})
endfunction()

function(liteanno_bundle_qt_translation target)
    if(NOT LITEANNO_QT_TRANSLATION)
        return()
    endif()
    if(NOT EXISTS "${LITEANNO_QT_TRANSLATION}")
        message(FATAL_ERROR "LITEANNO_QT_TRANSLATION: ${LITEANNO_QT_TRANSLATION} does not exist")
    endif()
    get_filename_component(_dir "${LITEANNO_QT_TRANSLATION}" DIRECTORY)
    qt_add_resources(${target} "${target}_i18n" PREFIX "/i18n" BASE "${_dir}" FILES "${LITEANNO_QT_TRANSLATION}")
endfunction()
