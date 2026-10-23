# The window, as a library so the tests can drive it.
add_library(liteanno_ui_lib STATIC
    Canvas.cpp
    Canvas.h
    MainWindow.cpp
    MainWindow.h
    Prompts.cpp
    Prompts.h
    Texts.cpp
    Texts.h
)
target_link_libraries(liteanno_ui_lib PUBLIC liteanno_application liteanno_imaging liteanno_core Qt6::Widgets)

qt_add_executable(liteanno_ui WIN32
    main.cpp
)
set_target_properties(liteanno_ui PROPERTIES OUTPUT_NAME LiteAnno)
target_link_libraries(liteanno_ui PRIVATE liteanno_ui_lib)

# The single-file Windows build (static Qt, .github/workflows/package.yml) has nothing next
# to the program: Qt's Chinese dialog texts and the licence texts of what is linked in are
# built into it, shown under "帮助 > 关于".
set(LITEANNO_QT_TRANSLATION "" CACHE FILEPATH "qtbase_zh_CN.qm to build into the program")
set(LITEANNO_BUNDLED_LICENSES_DIR "" CACHE PATH "Folder of licence texts to build into the program")
if(LITEANNO_QT_TRANSLATION)
    if(NOT EXISTS "${LITEANNO_QT_TRANSLATION}")
        message(FATAL_ERROR "LITEANNO_QT_TRANSLATION: ${LITEANNO_QT_TRANSLATION} does not exist")
    endif()
    get_filename_component(_liteanno_qm_dir "${LITEANNO_QT_TRANSLATION}" DIRECTORY)
    qt_add_resources(liteanno_ui "liteanno_i18n" PREFIX "/i18n" BASE "${_liteanno_qm_dir}"
        FILES "${LITEANNO_QT_TRANSLATION}")
endif()
if(LITEANNO_BUNDLED_LICENSES_DIR)
    file(GLOB_RECURSE _liteanno_licenses LIST_DIRECTORIES false "${LITEANNO_BUNDLED_LICENSES_DIR}/*")
    if(NOT _liteanno_licenses)
        message(FATAL_ERROR "No licence texts in ${LITEANNO_BUNDLED_LICENSES_DIR}")
    endif()
    qt_add_resources(liteanno_ui "liteanno_licenses" PREFIX "/licenses" BASE "${LITEANNO_BUNDLED_LICENSES_DIR}"
        FILES ${_liteanno_licenses})
endif()
