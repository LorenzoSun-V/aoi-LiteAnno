add_library(liteanno_formats STATIC
    AnnotationFile.cpp
    AnnotationFile.h
    AnnotationStore.cpp
    AnnotationStore.h
    JsonText.cpp
    JsonText.h
    ProjectFile.cpp
    ProjectFile.h
    RecoveryRecord.cpp
    RecoveryRecord.h
    Exporters.cpp
    Exporters.h
    Importers.cpp
    Importers.h
    PolygonRaster.cpp
    PolygonRaster.h
    PngFile.cpp
    PngFile.h
    WorksetFiles.cpp
    WorksetFiles.h
)
target_link_libraries(liteanno_formats
    PUBLIC liteanno_core Qt6::Core
    # JsonText.h exposes nlohmann/json; only formats and its tests include it.
    PRIVATE liteanno_nlohmann_json)
