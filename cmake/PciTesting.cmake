include(CMakeParseArguments)

function(pci_add_catch_test_target target)
    cmake_parse_arguments(ARG
            "WITH_MAIN;INTEGRATION"
            ""
            "SOURCES;LIBRARIES;INCLUDE_DIRECTORIES;PROPERTIES"
            ${ARGN})

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "${target} requires test sources")
    endif()

    set(project_test_libraries "")
    foreach(library IN LISTS ARG_LIBRARIES)
        pci_normalize_link_item(normalized_library "${library}")
        if(normalized_library MATCHES "^pcinspector_" AND
           NOT normalized_library MATCHES
               "^pcinspector_project_(options|warnings)$")
            list(APPEND project_test_libraries "${normalized_library}")
        endif()
    endforeach()
    list(LENGTH project_test_libraries project_test_library_count)
    if(NOT ARG_INTEGRATION AND NOT project_test_library_count EQUAL 1)
        message(FATAL_ERROR
                "${target} must link exactly one project target; use INTEGRATION for an explicitly cross-target test")
    endif()

    add_executable(${target} ${ARG_SOURCES})
    target_include_directories(${target} PRIVATE
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/tests"
            ${ARG_INCLUDE_DIRECTORIES})
    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES})
    if(ARG_WITH_MAIN)
        target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    else()
        target_link_libraries(${target} PRIVATE Catch2::Catch2)
    endif()
    set_target_properties(${target} PROPERTIES FOLDER Tests)
    # Test fixtures intentionally omit irrelevant aggregate fields. Production
    # targets retain the corresponding missing-initializer warning.
    set_target_properties(${target} PROPERTIES
            PCINSPECTOR_PARTIAL_AGGREGATE_FIXTURES TRUE)
    pci_configure_target(${target})

    # Discover at test time rather than build time. This keeps ordinary builds
    # side-effect free and lets cross-compiling test jobs supply an emulator.
    catch_discover_tests(${target}
            DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "${target}::"
            ADD_TAGS_AS_LABELS
            PROPERTIES
            ${ARG_PROPERTIES})
endfunction()
