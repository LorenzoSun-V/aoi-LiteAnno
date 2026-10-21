add_library(liteanno_dataset STATIC
    DirectoryScanner.cpp
    DirectoryScanner.h
    NaturalSort.cpp
    NaturalSort.h
)
target_link_libraries(liteanno_dataset PUBLIC liteanno_core Qt6::Core)
