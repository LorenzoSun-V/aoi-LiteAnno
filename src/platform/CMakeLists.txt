add_library(liteanno_platform STATIC
    VolumeBinding.cpp
    VolumeBinding.h
)
target_link_libraries(liteanno_platform PUBLIC liteanno_core Qt6::Core)
