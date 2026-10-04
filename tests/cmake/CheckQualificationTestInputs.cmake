foreach(required_variable IN ITEMS
        QUALIFICATION_FIXTURE_DIR
        QUALIFICATION_RUNNER
        QUALIFICATION_REGISTRATION_GUARD
        QUALIFICATION_REGISTRATION_GUARD_TEST)
    if(NOT DEFINED ${required_variable} OR ${required_variable} STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(required_qualification_inputs
    "${QUALIFICATION_FIXTURE_DIR}/tolerances.json"
    "${QUALIFICATION_FIXTURE_DIR}/release-h-local-two-file.json"
    "${QUALIFICATION_FIXTURE_DIR}/release-h-local-two-file-reopen.json"
    "${QUALIFICATION_FIXTURE_DIR}/release-h-native-metal-final.json"
    "${QUALIFICATION_RUNNER}"
    "${QUALIFICATION_REGISTRATION_GUARD}"
    "${QUALIFICATION_REGISTRATION_GUARD_TEST}")
foreach(required_file IN LISTS required_qualification_inputs)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
                "Required qualification test input is missing: ${required_file}")
    endif()
endforeach()
