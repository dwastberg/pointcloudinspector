foreach(required_variable IN ITEMS
        VALIDATOR
        SOURCE_FIXTURE_DIR
        RUNNER
        REGISTRATION_GUARD
        REGISTRATION_GUARD_TEST
        WORK_DIR
        MISSING_FILE)
    if(NOT DEFINED ${required_variable} OR ${required_variable} STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(fixture_dir "${WORK_DIR}/${MISSING_FILE}")
file(REMOVE_RECURSE "${fixture_dir}")
file(MAKE_DIRECTORY "${fixture_dir}")
file(COPY
     "${SOURCE_FIXTURE_DIR}/tolerances.json"
     "${SOURCE_FIXTURE_DIR}/release-h-local-two-file.json"
     "${SOURCE_FIXTURE_DIR}/release-h-local-two-file-reopen.json"
     "${SOURCE_FIXTURE_DIR}/release-h-native-metal-final.json"
     DESTINATION "${fixture_dir}")
file(REMOVE "${fixture_dir}/${MISSING_FILE}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DQUALIFICATION_FIXTURE_DIR=${fixture_dir}"
            "-DQUALIFICATION_RUNNER=${RUNNER}"
            "-DQUALIFICATION_REGISTRATION_GUARD=${REGISTRATION_GUARD}"
            "-DQUALIFICATION_REGISTRATION_GUARD_TEST=${REGISTRATION_GUARD_TEST}"
            -P "${VALIDATOR}"
    RESULT_VARIABLE validator_exit
    OUTPUT_VARIABLE validator_output
    ERROR_VARIABLE validator_error)
if(validator_exit EQUAL 0)
    message(FATAL_ERROR
            "missing fixture '${MISSING_FILE}' unexpectedly passed validation")
endif()
set(combined_output "${validator_output}\n${validator_error}")
string(FIND "${combined_output}" "${MISSING_FILE}" filename_position)
if(filename_position EQUAL -1)
    message(FATAL_ERROR
            "missing-input diagnostic did not name '${MISSING_FILE}':\n${combined_output}")
endif()
