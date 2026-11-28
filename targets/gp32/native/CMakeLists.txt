# Native GP32 project written by m0recomp from a deepeel code map.
cmake_minimum_required(VERSION 3.16)
project(m0boy_gp32_native C)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(app "${CMAKE_CURRENT_SOURCE_DIR}/app")
set(aot "${CMAKE_CURRENT_BINARY_DIR}/aot/app_aot.c")
# Replace app/app.code and app/app.image.bin (a longer exploration) and
# configure again to recompile.
set(inputs "${app}/app.code" "${app}/app.image.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/gp32/runtime/recompile.py")
set(stale FALSE)
foreach(input ${inputs})
    if("${input}" IS_NEWER_THAN "${aot}")
        set(stale TRUE)
    endif()
endforeach()
if(stale)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/aot")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gp32/runtime/recompile.py"
        "${app}/app.code" "${app}/app.image.bin" "${aot}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        file(REMOVE "${aot}")
        message(FATAL_ERROR "GP32 recompilation failed")
    endif()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${inputs})

set(M0_GP32_AOT "${aot}" CACHE FILEPATH "Recompiled application" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_subdirectory(gp32/runtime runtime)
