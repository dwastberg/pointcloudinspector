if(NOT FORMATTER OR NOT EXISTS "${FORMATTER}")
    message(FATAL_ERROR
            "clang-format 22.1.8 is required. Configure PCINSPECTOR_CLANG_FORMAT_EXECUTABLE to point to it.")
endif()
execute_process(COMMAND "${FORMATTER}" --version
    OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT version MATCHES "version 22\\.1\\.8($|[ \t])")
    message(FATAL_ERROR "clang-format 22.1.8 is required; found: ${version}")
endif()

if(MODE STREQUAL "format")
    set(arguments -i)
elseif(MODE STREQUAL "format-check")
    set(arguments --dry-run --Werror)
else()
    message(FATAL_ERROR "Unknown formatting mode: ${MODE}")
endif()
file(GLOB_RECURSE sources
     "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.h"
     "${PROJECT_SOURCE_DIR}/tests/*.cpp" "${PROJECT_SOURCE_DIR}/tests/*.h"
     "${PROJECT_SOURCE_DIR}/tools/*.cpp" "${PROJECT_SOURCE_DIR}/tools/*.h")
list(APPEND sources "${PROJECT_SOURCE_DIR}/main.cpp")
execute_process(COMMAND "${FORMATTER}" ${arguments} ${sources}
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${MODE} failed with exit code ${result}")
endif()
