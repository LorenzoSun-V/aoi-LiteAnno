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

liteanno_bundle_qt_translation(liteanno_ui)
liteanno_bundle_licenses(liteanno_ui)
