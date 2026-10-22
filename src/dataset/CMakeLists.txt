add_library(liteanno_dataset STATIC
    Catalog.h
    CatalogReader.cpp
    CatalogReader.h
    CatalogWriter.cpp
    CatalogWriter.h
    DirectoryScanner.cpp
    DirectoryScanner.h
    NaturalSort.cpp
    NaturalSort.h
    Sqlite.h
)
find_package(Threads REQUIRED)
target_link_libraries(liteanno_dataset
    PUBLIC liteanno_core Qt6::Core
    PRIVATE liteanno_sqlite Threads::Threads)
