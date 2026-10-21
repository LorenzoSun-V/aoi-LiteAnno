# Toolchain pinning (architecture section 12).
#
# CI and release builds use exactly these versions. Upgrading either one, including a
# Visual Studio update that only changes the compiler build within toolset v143, means a new
# acceptance run on the oldest supported Windows 10 build. Keep QT_VERSION in
# .github/workflows/ci.yml in sync with LITEANNO_PINNED_QT_VERSION.

set(LITEANNO_PINNED_QT_VERSION "6.8.3")
# Visual Studio 2022 17.14 (MSVC toolset v143), compiler cl.exe 19.44.35229.
set(LITEANNO_PINNED_MSVC_TOOLSET "143")
set(LITEANNO_PINNED_MSVC_VERSION "19.44.35229.0")

option(LITEANNO_ALLOW_UNPINNED_TOOLCHAIN
    "Allow a Qt version or MSVC compiler other than the pinned ones (local development only, never for releases)"
    OFF)

# Sets <out_var> to the list of differences from the pinned toolchain; empty when it matches.
# Takes the values as arguments so tests/cmake/toolchain_pin_test.cmake can check it.
function(liteanno_toolchain_pin_problems out_var qt_version is_msvc msvc_toolset compiler_version)
    set(_problems "")
    if(NOT qt_version VERSION_EQUAL LITEANNO_PINNED_QT_VERSION)
        list(APPEND _problems "found Qt ${qt_version}, pinned Qt is ${LITEANNO_PINNED_QT_VERSION}")
    endif()
    if(is_msvc)
        if(NOT msvc_toolset STREQUAL LITEANNO_PINNED_MSVC_TOOLSET)
            list(APPEND _problems
                "found MSVC toolset v${msvc_toolset}, pinned toolset is v${LITEANNO_PINNED_MSVC_TOOLSET}")
        endif()
        if(NOT compiler_version VERSION_EQUAL LITEANNO_PINNED_MSVC_VERSION)
            list(APPEND _problems
                "found MSVC ${compiler_version}, pinned MSVC is ${LITEANNO_PINNED_MSVC_VERSION}")
        endif()
    endif()
    set(${out_var} "${_problems}" PARENT_SCOPE)
endfunction()

function(liteanno_check_toolchain_pin)
    message(STATUS "LiteAnno toolchain: Qt ${Qt6_VERSION}, ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

    set(_is_msvc OFF)
    if(MSVC)
        set(_is_msvc ON)
    endif()
    liteanno_toolchain_pin_problems(_problems "${Qt6_VERSION}" ${_is_msvc} "${MSVC_TOOLSET_VERSION}"
                                    "${CMAKE_CXX_COMPILER_VERSION}")
    if(NOT _problems)
        return()
    endif()

    list(JOIN _problems "; " _text)
    if(LITEANNO_ALLOW_UNPINNED_TOOLCHAIN)
        message(WARNING "Unpinned toolchain (${_text}). Do not use this build for releases.")
    else()
        message(FATAL_ERROR
            "Unpinned toolchain (${_text}). Install the pinned versions, or configure with "
            "-DLITEANNO_ALLOW_UNPINNED_TOOLCHAIN=ON for a local development build.")
    endif()
endfunction()
