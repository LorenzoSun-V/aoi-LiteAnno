configure_file(Version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/core/Version.h" @ONLY)

add_library(liteanno_core STATIC
    AnnotationDocument.cpp
    AnnotationDocument.h
    Diagnostic.cpp
    Diagnostic.h
    FormatVersion.h
    Geometry.cpp
    Geometry.h
    ImageInfo.h
    ReviewState.h
    Shape.h
    ShapeType.cpp
    ShapeType.h
    "${CMAKE_CURRENT_BINARY_DIR}/generated/core/Version.h"
)
target_include_directories(liteanno_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(liteanno_core PUBLIC Qt6::Core)
