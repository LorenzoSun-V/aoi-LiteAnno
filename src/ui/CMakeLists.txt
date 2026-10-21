qt_add_executable(liteanno_ui WIN32
    main.cpp
)
set_target_properties(liteanno_ui PROPERTIES OUTPUT_NAME LiteAnno)
target_link_libraries(liteanno_ui PRIVATE liteanno_core Qt6::Widgets)
