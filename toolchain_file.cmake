set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES TOS_PYTHON)
if(TOS_PYTHON)
    set(_gd32_python "${TOS_PYTHON}")
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_gd32_python "${Python3_EXECUTABLE}")
endif()
# Resolve through the same code as platform_prepare.py. This is read-only:
# the prepare child cannot export environment changes to its parent CMake.
execute_process(
    COMMAND "${_gd32_python}" "${CMAKE_CURRENT_LIST_DIR}/toolchain_prepare.py" --resolve
    RESULT_VARIABLE _gd32_result
    OUTPUT_VARIABLE _gd32_bin
    ERROR_VARIABLE _gd32_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _gd32_result EQUAL 0)
    message(FATAL_ERROR "Cannot resolve GD32H77X ARM GCC: ${_gd32_error}")
endif()
foreach(_tool gcc g++ ar ranlib objcopy size ld as)
    # Do not allow another installation on PATH or an old CMake cache entry
    # to supply any member of this toolchain.
    unset(_gd32_tool CACHE)
    unset(_gd32_tool)
    find_program(_gd32_tool NAMES arm-none-eabi-${_tool}
        PATHS "${_gd32_bin}" NO_DEFAULT_PATH REQUIRED)
    set(_gd32_${_tool} "${_gd32_tool}")
endforeach()
foreach(_pair C:gcc CXX:g++ ASM:gcc)
    string(REPLACE ":" ";" _parts "${_pair}")
    list(GET _parts 0 _language)
    list(GET _parts 1 _tool)
    set(CMAKE_${_language}_COMPILER "${_gd32_${_tool}}")
endforeach()
unset(_gd32_tool CACHE)
set(CMAKE_AR "${_gd32_ar}")
set(CMAKE_RANLIB "${_gd32_ranlib}")
set(CMAKE_OBJCOPY "${_gd32_objcopy}")
set(CMAKE_SIZE "${_gd32_size}")
set(CMAKE_LINKER "${_gd32_ld}")
set(_gd32_flags "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=softfp -ffunction-sections -fdata-sections -Os -g3")
set(CMAKE_C_FLAGS "${_gd32_flags}")
set(CMAKE_CXX_FLAGS "${_gd32_flags} -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS "${_gd32_flags}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

