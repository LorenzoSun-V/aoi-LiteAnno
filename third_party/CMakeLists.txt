# nlohmann/json 3.12.0 (MIT), single header. Used only inside src/formats and its tests.
add_library(liteanno_nlohmann_json INTERFACE)
target_include_directories(liteanno_nlohmann_json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann/include")
target_compile_definitions(liteanno_nlohmann_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=0)
