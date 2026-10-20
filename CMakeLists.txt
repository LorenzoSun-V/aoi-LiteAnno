cmake_minimum_required(VERSION 3.22)

project(aoi-LiteAnno
    VERSION 0.1.0
    DESCRIPTION "Lightweight image annotation tool compatible with X-AnyLabeling JSON files"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LiteAnnoToolchainPin)
include(LiteAnnoDependencyRules)

option(LITEANNO_BUILD_UI "Build the desktop application" ON)
option(LITEANNO_BUILD_TESTS "Build the tests" ON)

set(_liteanno_qt_components Core)
if(LITEANNO_BUILD_UI)
    list(APPEND _liteanno_qt_components Gui Widgets)
endif()
if(LITEANNO_BUILD_TESTS)
    list(APPEND _liteanno_qt_components Test)
endif()
find_package(Qt6 REQUIRED COMPONENTS ${_liteanno_qt_components})
liteanno_check_toolchain_pin()
qt_standard_project_setup()

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(LITEANNO_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(src)
if(LITEANNO_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Architecture section 13: core does not depend on Qt Gui/Widgets, and the modules on the
# same layer (dataset, imaging, formats, platform) depend only on core, never on each other.
liteanno_forbid_dependencies(liteanno_core
    FORBIDDEN Qt6::Gui Qt6::Widgets
              liteanno_formats liteanno_dataset liteanno_imaging liteanno_platform)
liteanno_forbid_dependencies(liteanno_formats
    FORBIDDEN Qt6::Gui Qt6::Widgets
              liteanno_dataset liteanno_imaging liteanno_platform)
