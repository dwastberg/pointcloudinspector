if(NOT DEFINED BUILD_DIR OR BUILD_DIR STREQUAL "")
    message(FATAL_ERROR "BUILD_DIR is required")
endif()
if(NOT DEFINED CTEST_CONFIGURATION OR CTEST_CONFIGURATION STREQUAL "")
    message(FATAL_ERROR "CTEST_CONFIGURATION is required")
endif()

cmake_path(ABSOLUTE_PATH BUILD_DIR NORMALIZE OUTPUT_VARIABLE absolute_build_dir)
if(NOT IS_DIRECTORY "${absolute_build_dir}")
    message(FATAL_ERROR
            "BUILD_DIR does not exist: '${absolute_build_dir}'")
endif()
if(NOT EXISTS "${absolute_build_dir}/CTestTestfile.cmake")
    message(FATAL_ERROR
            "BUILD_DIR has no CTest manifest: '${absolute_build_dir}'")
endif()

if(DEFINED CTEST_EXECUTABLE AND NOT CTEST_EXECUTABLE STREQUAL "")
    set(ctest_command "${CTEST_EXECUTABLE}")
else()
    get_filename_component(cmake_bin_dir "${CMAKE_COMMAND}" DIRECTORY)
    set(ctest_command "${cmake_bin_dir}/ctest${CMAKE_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${ctest_command}")
        find_program(ctest_command NAMES ctest REQUIRED)
    endif()
endif()
if(NOT EXISTS "${ctest_command}")
    message(FATAL_ERROR "CTest executable does not exist: '${ctest_command}'")
endif()

execute_process(
    COMMAND "${ctest_command}"
            --test-dir "${absolute_build_dir}"
            -C "${CTEST_CONFIGURATION}"
            --show-only=json-v1
            -R "^qualification_diff_"
    RESULT_VARIABLE ctest_exit
    OUTPUT_VARIABLE ctest_json
    ERROR_VARIABLE ctest_error)
if(NOT ctest_exit EQUAL 0)
    message(FATAL_ERROR
            "CTest inventory failed with exit ${ctest_exit}:\n${ctest_error}")
endif()

string(JSON inventory_kind GET "${ctest_json}" kind)
if(NOT inventory_kind STREQUAL "ctestInfo")
    message(FATAL_ERROR "unexpected CTest inventory kind '${inventory_kind}'")
endif()
string(JSON test_count LENGTH "${ctest_json}" tests)
if(test_count EQUAL 0)
    message(FATAL_ERROR "qualification registration is empty")
endif()

set(expected_names
    qualification_diff_release-h-local-two-file_self
    qualification_diff_release-h-local-two-file-reopen_self
    qualification_diff_release-h-native-metal-final_self
    qualification_diff_ignored_self_check
    qualification_diff_regression_self_check
    qualification_diff_mismatch_self_check
    qualification_diff_missing_self_check)
set(actual_names)
math(EXPR last_test "${test_count} - 1")
foreach(test_index RANGE 0 ${last_test})
    string(JSON test_name GET "${ctest_json}" tests ${test_index} name)
    list(FIND actual_names "${test_name}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR
                "qualification registration contains duplicate '${test_name}'")
    endif()
    list(APPEND actual_names "${test_name}")

    string(JSON property_count LENGTH "${ctest_json}"
           tests ${test_index} properties)
    if(property_count GREATER 0)
        math(EXPR last_property "${property_count} - 1")
        foreach(property_index RANGE 0 ${last_property})
            string(JSON property_name GET "${ctest_json}"
                   tests ${test_index} properties ${property_index} name)
            if(property_name STREQUAL "DISABLED")
                string(JSON property_value GET "${ctest_json}"
                       tests ${test_index} properties ${property_index} value)
                if(property_value)
                    message(FATAL_ERROR
                            "qualification test '${test_name}' is disabled")
                endif()
            endif()
        endforeach()
    endif()
endforeach()

list(SORT expected_names)
list(SORT actual_names)
if(NOT actual_names STREQUAL expected_names)
    message(FATAL_ERROR
            "qualification registration mismatch\nexpected: ${expected_names}\nactual: ${actual_names}")
endif()

message(STATUS
        "Verified ${test_count} enabled qualification-diff tests: ${actual_names}")
