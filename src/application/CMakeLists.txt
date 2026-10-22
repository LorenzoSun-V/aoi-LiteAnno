add_library(liteanno_application STATIC
    BackgroundSaver.cpp
    BackgroundSaver.h
    DocumentSession.cpp
    DocumentSession.h
    FileStatus.cpp
    FileStatus.h
    LocalDataPaths.cpp
    LocalDataPaths.h
    ProjectLock.cpp
    ProjectCatalog.cpp
    ProjectCatalog.h
    ProjectLock.h
    ProjectSession.cpp
    ProjectSession.h
    RecoveryService.cpp
    RecoveryService.h
    SaveCoordinator.cpp
    SaveCoordinator.h
)
find_package(Threads REQUIRED)
target_link_libraries(liteanno_application
    PUBLIC liteanno_formats liteanno_platform liteanno_dataset liteanno_core Qt6::Core
    PRIVATE Threads::Threads)
