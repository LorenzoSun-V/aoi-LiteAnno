# Checks liteanno_toolchain_pin_problems with simulated toolchains.
# Run as: cmake -P toolchain_pin_test.cmake

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/LiteAnnoToolchainPin.cmake")

# expect(<name> <expected problem count> <qt> <is_msvc> <toolset> <compiler version>)
function(expect name expected qt is_msvc toolset version)
    liteanno_toolchain_pin_problems(_problems "${qt}" ${is_msvc} "${toolset}" "${version}")
    list(LENGTH _problems _count)
    if(NOT _count EQUAL expected)
        message(SEND_ERROR "${name}: expected ${expected} problem(s), got ${_count}: ${_problems}")
    endif()
endfunction()

set(_qt "${LITEANNO_PINNED_QT_VERSION}")
set(_msvc "${LITEANNO_PINNED_MSVC_VERSION}")
set(_toolset "${LITEANNO_PINNED_MSVC_TOOLSET}")

expect("pinned MSVC toolchain" 0 "${_qt}" ON "${_toolset}" "${_msvc}")
expect("pinned Qt with GCC" 0 "${_qt}" OFF "" "13.3.0")
expect("other Qt patch" 1 "6.8.2" ON "${_toolset}" "${_msvc}")
# Newer and older compilers that still belong to toolset v143.
expect("newer v143 compiler" 1 "${_qt}" ON "${_toolset}" "19.44.35230.0")
expect("older v143 compiler" 1 "${_qt}" ON "${_toolset}" "19.34.31937.0")
expect("other toolset" 2 "${_qt}" ON "145" "19.50.35717.0")
