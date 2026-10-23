# liteanno-make-icon with a logo and an output in a folder whose name Windows code pages
# cannot spell (Hangul, Cyrillic, CJK), as a user's build or logo folder may be.
# cmake -DTOOL=<liteanno-make-icon> -DLOGO=<png> -DWORK=<folder> -P make_icon_paths.cmake
set(dir "${WORK}/한글-Ж-图标 logo")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
file(COPY_FILE "${LOGO}" "${dir}/标志.png")
execute_process(COMMAND "${TOOL}" "${dir}/标志.png" "${dir}/图标.ico" RESULT_VARIABLE result
    OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "liteanno-make-icon failed (${result}): ${out}${err}")
endif()
file(SIZE "${dir}/图标.ico" size)
if(size LESS 1000)
    message(FATAL_ERROR "the icon is only ${size} bytes")
endif()
file(READ "${dir}/图标.ico" header LIMIT 6 HEX)
if(NOT header STREQUAL "000001000700")
    message(FATAL_ERROR "not an icon of 7 sizes: ${header}")
endif()
message(STATUS "made ${dir}/图标.ico (${size} bytes)")
