# The program's logo: one PNG file for the window icon, the logo inside the program and the
# Windows .exe icon. To use another one, replace branding/logo.png or point LITEANNO_LOGO at
# your file (branding/README.md).
set(LITEANNO_LOGO "${PROJECT_SOURCE_DIR}/branding/logo.png" CACHE FILEPATH
    "The program's logo: a PNG, square, 256 x 256 pixels or more (branding/README.md)")
if(NOT EXISTS "${LITEANNO_LOGO}")
    message(FATAL_ERROR "LITEANNO_LOGO: ${LITEANNO_LOGO} does not exist")
endif()
get_filename_component(_liteanno_logo_ext "${LITEANNO_LOGO}" LAST_EXT)
string(TOLOWER "${_liteanno_logo_ext}" _liteanno_logo_ext)
if(NOT _liteanno_logo_ext STREQUAL ".png")
    message(FATAL_ERROR "LITEANNO_LOGO must be a .png file (got ${LITEANNO_LOGO})")
endif()

# Builds the logo into `target` as a Qt resource, :/liteanno/logo.png (ui/Theme.h appIcon).
function(liteanno_embed_logo target)
    set_source_files_properties("${LITEANNO_LOGO}" PROPERTIES QT_RESOURCE_ALIAS "logo.png")
    qt_add_resources(${target} "${target}_logo" PREFIX "/liteanno" FILES "${LITEANNO_LOGO}")
endfunction()

# On Windows, gives the program `target` the logo as its .exe icon (made by imaging/make_icon.cpp).
function(liteanno_exe_icon target)
    if(WIN32)
        target_sources(${target} PRIVATE "${LITEANNO_LOGO_RC}")
        set_source_files_properties("${LITEANNO_LOGO_RC}" TARGET_DIRECTORY ${target} PROPERTIES
            OBJECT_DEPENDS "${LITEANNO_LOGO_ICO}")
        add_dependencies(${target} liteanno_logo_icon)
    endif()
endfunction()
