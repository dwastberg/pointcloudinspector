find_program(PCINSPECTOR_CLANG_FORMAT_EXECUTABLE
             NAMES clang-format
             DOC "clang-format executable used by format targets")

foreach(mode IN ITEMS format format-check)
    add_custom_target(${mode}
        COMMAND "${CMAKE_COMMAND}"
                "-DFORMATTER=${PCINSPECTOR_CLANG_FORMAT_EXECUTABLE}"
                "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                "-DMODE=${mode}"
                -P "${CMAKE_CURRENT_LIST_DIR}/RunFormatting.cmake"
        COMMENT "Running ${mode} with clang-format 22.1.8"
        VERBATIM)
endforeach()
