# nlohmann/json 3.12.0 (MIT), single header. Used only inside src/formats and its tests.
add_library(liteanno_nlohmann_json INTERFACE)
target_include_directories(liteanno_nlohmann_json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann/include")
target_compile_definitions(liteanno_nlohmann_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=0)

# SQLite (public domain) for the local catalog (architecture 6.3), compiled from its source.
# The version and the SHA-512 are the ones vcpkg's sqlite3 port uses.
option(LITEANNO_SYSTEM_SQLITE
    "Use the SQLite library of the system instead of the pinned source (local development only)"
    OFF)
add_library(liteanno_sqlite INTERFACE)
if(LITEANNO_SYSTEM_SQLITE)
    if(NOT LITEANNO_ALLOW_UNPINNED_TOOLCHAIN)
        message(FATAL_ERROR "LITEANNO_SYSTEM_SQLITE needs -DLITEANNO_ALLOW_UNPINNED_TOOLCHAIN=ON: "
                            "release builds use the pinned SQLite source.")
    endif()
    find_package(SQLite3 REQUIRED)
    message(WARNING "Using the system SQLite ${SQLite3_VERSION}. Do not use this build for releases.")
    target_link_libraries(liteanno_sqlite INTERFACE SQLite::SQLite3)
else()
    enable_language(C)
    include(FetchContent)
    set(LITEANNO_SQLITE_VERSION "3.54.1")
    FetchContent_Declare(liteanno_sqlite_source
        URL "https://sqlite.org/2026/sqlite-autoconf-3540100.tar.gz"
        URL_HASH SHA512=3df4348f54f5cd0554eec6d91e1d08a70535b7b78f497b2c6f24910b5087f6ef70483c9e35fa388bd8f38483cb8d1cb84e98ccdd58b9f525a10e0cf48c8c6742
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    # The archive has no CMakeLists.txt, so this only downloads, checks and unpacks it.
    FetchContent_MakeAvailable(liteanno_sqlite_source)
    add_library(liteanno_sqlite3 STATIC "${liteanno_sqlite_source_SOURCE_DIR}/sqlite3.c")
    # Only the header is put on the include path: the archive also has a file named VERSION,
    # which <version> finds on file systems that ignore case.
    configure_file("${liteanno_sqlite_source_SOURCE_DIR}/sqlite3.h"
                   "${CMAKE_CURRENT_BINARY_DIR}/sqlite-include/sqlite3.h" COPYONLY)
    target_include_directories(liteanno_sqlite3 SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/sqlite-include")
    # One write connection on its own thread, read connections elsewhere: serialized mode
    # is not needed, multi-thread mode is. No extensions are loaded.
    target_compile_definitions(liteanno_sqlite3 PRIVATE
        SQLITE_THREADSAFE=2 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DQS=0)
    set_target_properties(liteanno_sqlite3 PROPERTIES C_STANDARD 99)
    if(MSVC)
        target_compile_options(liteanno_sqlite3 PRIVATE /W0)
    else()
        target_compile_options(liteanno_sqlite3 PRIVATE -w)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(liteanno_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    target_link_libraries(liteanno_sqlite INTERFACE liteanno_sqlite3)
endif()
