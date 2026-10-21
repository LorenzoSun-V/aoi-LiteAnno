add_library(liteanno_imaging STATIC
    ImageLoader.cpp
    ImageLoader.h
)
target_link_libraries(liteanno_imaging PUBLIC liteanno_core Qt6::Gui)
