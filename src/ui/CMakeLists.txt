# The window, as a library so the tests can drive it.
add_library(liteanno_ui_lib STATIC
    Canvas.cpp
    Canvas.h
    ListDelegates.cpp
    ListDelegates.h
    MainWindow.cpp
    MainWindow.h
    Prompts.cpp
    Prompts.h
    Texts.cpp
    Texts.h
    Theme.cpp
    Theme.h
)
target_link_libraries(liteanno_ui_lib PUBLIC liteanno_application liteanno_imaging liteanno_core Qt6::Widgets)

# The style sheet and the icons (rendered from icons/svg by icons/render.sh).
file(GLOB LITEANNO_ICON_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/png/*.png")
foreach(icon IN LISTS LITEANNO_ICON_FILES)
    get_filename_component(icon_name "${icon}" NAME)
    set_source_files_properties("${icon}" PROPERTIES QT_RESOURCE_ALIAS "icons/${icon_name}")
endforeach()
qt_add_resources(liteanno_ui_lib liteanno_ui_theme
    PREFIX "/liteanno"
    FILES theme/light.qss ${LITEANNO_ICON_FILES}
)

qt_add_executable(liteanno_ui WIN32
    main.cpp
)
set_target_properties(liteanno_ui PROPERTIES OUTPUT_NAME LiteAnno)
target_link_libraries(liteanno_ui PRIVATE liteanno_ui_lib)

liteanno_bundle_qt_translation(liteanno_ui)
liteanno_bundle_licenses(liteanno_ui)
