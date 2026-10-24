# liteanno-cli (architecture 13). Reads image sizes with the same codecs as the window.
qt_add_executable(liteanno_cli
    main.cpp
)
set_target_properties(liteanno_cli PROPERTIES OUTPUT_NAME liteanno-cli)
target_link_libraries(liteanno_cli PRIVATE liteanno_application liteanno_imaging liteanno_core Qt6::Core)
liteanno_exe_icon(liteanno_cli)
liteanno_version_info(liteanno_cli liteanno-cli.exe "aoi-LiteAnno 命令行工具")
liteanno_bundle_licenses(liteanno_cli)
