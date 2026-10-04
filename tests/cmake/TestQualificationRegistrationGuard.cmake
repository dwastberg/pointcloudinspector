foreach(required_variable IN ITEMS MODE CHECK_SCRIPT CTEST_EXECUTABLE WORK_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()
if(NOT EXISTS "${CHECK_SCRIPT}")
    message(FATAL_ERROR "CHECK_SCRIPT does not exist: '${CHECK_SCRIPT}'")
endif()

set(expected_names
    qualification_diff_release-h-local-two-file_self
    qualification_diff_release-h-local-two-file-reopen_self
    qualification_diff_release-h-native-metal-final_self
    qualification_diff_ignored_self_check
    qualification_diff_regression_self_check
    qualification_diff_mismatch_self_check
    qualification_diff_missing_self_check)
set(fake_names ${expected_names})
set(expected_guard_success FALSE)
set(expected_message "qualification registration")
if(MODE STREQUAL "valid")
    set(expected_guard_success TRUE)
elseif(MODE STREQUAL "zero")
    set(fake_names)
    set(expected_message "qualification registration is empty")
elseif(MODE STREQUAL "missing")
    list(REMOVE_AT fake_names 0)
    set(expected_message "qualification registration mismatch")
elseif(MODE STREQUAL "unexpected")
    list(REMOVE_AT fake_names 0)
    list(APPEND fake_names qualification_diff_unexpected_self_check)
    set(expected_message "qualification registration mismatch")
elseif(MODE STREQUAL "disabled")
    set(expected_message "disabled")
else()
    message(FATAL_ERROR "unsupported MODE '${MODE}'")
endif()

set(fake_build "${WORK_DIR}/${MODE}")
file(REMOVE_RECURSE "${fake_build}")
file(MAKE_DIRECTORY "${fake_build}")
file(WRITE "${fake_build}/CTestTestfile.cmake"
     "# Generated qualification registration guard fixture.\n")
foreach(test_name IN LISTS fake_names)
    file(APPEND "${fake_build}/CTestTestfile.cmake"
         "add_test(${test_name} \"${CMAKE_COMMAND}\" -E true)\n")
endforeach()
if(MODE STREQUAL "disabled")
    list(GET expected_names 0 disabled_name)
    file(APPEND "${fake_build}/CTestTestfile.cmake"
         "set_tests_properties(${disabled_name} PROPERTIES DISABLED TRUE)\n")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${fake_build}"
            -DCTEST_CONFIGURATION=Debug
            "-DCTEST_EXECUTABLE=${CTEST_EXECUTABLE}"
            -P "${CHECK_SCRIPT}"
    RESULT_VARIABLE guard_exit
    OUTPUT_VARIABLE guard_output
    ERROR_VARIABLE guard_error)
set(combined_output "${guard_output}\n${guard_error}")

if(expected_guard_success)
    if(NOT guard_exit EQUAL 0)
        message(FATAL_ERROR
                "valid registration failed with exit ${guard_exit}:\n${combined_output}")
    endif()
else()
    if(guard_exit EQUAL 0)
        message(FATAL_ERROR
                "invalid '${MODE}' registration unexpectedly passed")
    endif()
    string(FIND "${combined_output}" "${expected_message}" message_position)
    if(message_position EQUAL -1)
        message(FATAL_ERROR
                "${MODE} failure did not contain '${expected_message}':\n${combined_output}")
    endif()
endif()
