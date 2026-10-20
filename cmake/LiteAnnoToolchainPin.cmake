# Toolchain pinning (architecture section 12).
#
# CI and release builds use exactly these versions. Upgrading either one means a new
# acceptance run on the oldest supported Windows 10 build. Keep QT_VERSION in
# .github/workflows/ci.yml in sync with LITEANNO_PINNED_QT_VERSION.

set(LITEANNO_PINNED_QT_VERSION "6.8.3")
# Visual Studio 2022 (MSVC toolset v143). The exact compiler build is printed at configure time.
set(LITEANNO_PINNED_MSVC_TOOLSET "143")

option(LITEANNO_ALLOW_UNPINNED_TOOLCHAIN
    "Allow a Qt version or MSVC toolset other than the pinned ones (local development only, never for releases)"
    OFF)

function(liteanno_check_toolchain_pin)
    message(STATUS "LiteAnno toolchain: Qt ${Qt6_VERSION}, ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

    set(_problems "")
    if(NOT Qt6_VERSION VERSION_EQUAL LITEANNO_PINNED_QT_VERSION)
        list(APPEND _problems "found Qt ${Qt6_VERSION}, pinned Qt is ${LITEANNO_PINNED_QT_VERSION}")
    endif()
    if(MSVC AND NOT MSVC_TOOLSET_VERSION STREQUAL LITEANNO_PINNED_MSVC_TOOLSET)
        list(APPEND _problems
            "found MSVC toolset v${MSVC_TOOLSET_VERSION}, pinned toolset is v${LITEANNO_PINNED_MSVC_TOOLSET}")
    endif()
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
