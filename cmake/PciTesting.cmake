include(CMakeParseArguments)

function(pci_add_catch_test_target target)
    cmake_parse_arguments(ARG
            "WITH_MAIN;INTEGRATION;ISOLATED;LONG_STRESS"
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
            PCINSPECTOR_PARTIAL_AGGREGATE_FIXTURES TRUE
            PCINSPECTOR_CATCH_TEST TRUE)
    pci_configure_target(${target})

    if(PCINSPECTOR_BATCH_TESTS AND NOT ARG_ISOLATED)
        # Keep each library's executable, but amortize Qt, sanitizer, and
        # fixture startup over all of its ordinary cases. Individual names
        # and assertions remain visible in the console and JUnit reports.
        file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/Testing/Reports")
        add_test(NAME "${target}"
                COMMAND ${target} "~[long-stress]"
                    --reporter console
                    --reporter "JUnit::out=${PROJECT_BINARY_DIR}/Testing/Reports/${target}-$<CONFIG>.xml")
        set_tests_properties("${target}" PROPERTIES
                LABELS "batch;${target}"
                ${ARG_PROPERTIES})
    else()
        # Discovery remains available for developer filtering and for cases
        # whose memory measurements or graphics state require isolation.
        catch_discover_tests(${target}
                DISCOVERY_MODE PRE_TEST
                TEST_SPEC "~[long-stress]"
                TEST_PREFIX "${target}::"
                ADD_TAGS_AS_LABELS
                PROPERTIES ${ARG_PROPERTIES})
    endif()
    if(ARG_LONG_STRESS AND PCINSPECTOR_ENABLE_LONG_STRESS_TESTS)
        catch_discover_tests(${target}
                DISCOVERY_MODE PRE_TEST
                TEST_SPEC "[long-stress]"
                TEST_PREFIX "${target}::"
                ADD_TAGS_AS_LABELS
                PROPERTIES ${ARG_PROPERTIES} RUN_SERIAL TRUE)
    endif()
endfunction()
