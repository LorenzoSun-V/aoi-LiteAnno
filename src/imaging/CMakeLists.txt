add_library(liteanno_imaging STATIC
    IconFile.cpp
    IconFile.h
    ImageLoader.cpp
    ImageLoader.h
)
target_link_libraries(liteanno_imaging PUBLIC liteanno_core Qt6::Gui)

# The Windows icon of both programs, made from the logo (cmake/LiteAnnoLogo.cmake).
add_executable(liteanno_make_icon make_icon.cpp)
set_target_properties(liteanno_make_icon PROPERTIES OUTPUT_NAME liteanno-make-icon)
target_link_libraries(liteanno_make_icon PRIVATE liteanno_imaging)
if(WIN32)
    set(_icon "${CMAKE_CURRENT_BINARY_DIR}/logo.ico")
    add_custom_command(OUTPUT "${_icon}"
        COMMAND liteanno_make_icon "${LITEANNO_LOGO}" "${_icon}"
        DEPENDS liteanno_make_icon "${LITEANNO_LOGO}"
        COMMENT "Making the program icon from ${LITEANNO_LOGO}"
        VERBATIM)
    add_custom_target(liteanno_logo_icon DEPENDS "${_icon}")
    file(TO_CMAKE_PATH "${_icon}" _icon_path)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/logo.rc" "IDI_ICON1 ICON \"${_icon_path}\"\n")
    set(LITEANNO_LOGO_RC "${CMAKE_CURRENT_BINARY_DIR}/logo.rc" CACHE INTERNAL "")
    set(LITEANNO_LOGO_ICO "${_icon}" CACHE INTERNAL "")
endif()
