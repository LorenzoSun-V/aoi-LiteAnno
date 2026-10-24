# The Windows version information of a program (Explorer > Properties > Details): product
# name, version, description and, when set, the company. Programs without it look less
# trustworthy to virus scanners, and signed programs show it next to the signature.
set(LITEANNO_COMPANY "" CACHE STRING
    "Company name in the Windows version information of the programs; empty leaves it out")
set(LITEANNO_BUILD_NUMBER "0" CACHE STRING
    "Fourth part of the Windows file version (the Package workflow passes its run number)")
if(NOT LITEANNO_BUILD_NUMBER MATCHES "^[0-9]+$" OR LITEANNO_BUILD_NUMBER GREATER 65535)
    message(FATAL_ERROR "LITEANNO_BUILD_NUMBER must be a number from 0 to 65535 (got ${LITEANNO_BUILD_NUMBER})")
endif()

# Gives the program `target` (file name `file_name`) its version information on Windows.
function(liteanno_version_info target file_name description)
    if(NOT WIN32)
        return()
    endif()
    # Text in the resource is UTF-8 (#pragma code_page in the template); quotes are doubled.
    foreach(name IN ITEMS file_name description)
        string(REPLACE "\"" "\"\"" ${name} "${${name}}")
    endforeach()
    string(REPLACE "\"" "\"\"" company "${LITEANNO_COMPANY}")
    set(LITEANNO_RC_COMPANY_LINE "")
    if(NOT company STREQUAL "")
        set(LITEANNO_RC_COMPANY_LINE "VALUE \"CompanyName\", \"${company}\"")
    endif()
    set(LITEANNO_RC_FILE_NAME "${file_name}")
    set(LITEANNO_RC_DESCRIPTION "${description}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${PROJECT_SOURCE_DIR}/cmake/LiteAnnoVersionInfo.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
endfunction()
