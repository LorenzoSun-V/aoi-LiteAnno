add_library(liteanno_application STATIC
    LocalDataPaths.cpp
    LocalDataPaths.h
    ProjectLock.cpp
    ProjectLock.h
    RecoveryService.cpp
    RecoveryService.h
    SaveCoordinator.cpp
    SaveCoordinator.h
)
target_link_libraries(liteanno_application PUBLIC liteanno_formats liteanno_platform liteanno_core Qt6::Core)
