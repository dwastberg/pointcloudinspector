foreach(required_variable IN ITEMS
        QUALIFICATION_DIFF
        BASELINE
        TOLERANCES
        MODE
        EXPECTED_EXIT
        EXPECTED_OUTPUT)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(READ "${BASELINE}" candidate_json)
if(MODE STREQUAL "ignored")
    string(REPLACE
           "\"timestamp_utc\": \"2026-07-20T20:09:02.491Z\""
           "\"timestamp_utc\": \"2099-01-01T00:00:00.000Z\""
           candidate_json "${candidate_json}")
    string(REPLACE
           "\"path\": \"test_data/3445-343.laz\""
           "\"path\": \"another/location/source-a.laz\""
           candidate_json "${candidate_json}")
    set(search_text "\"timestamp_utc\": \"2099-01-01T00:00:00.000Z\"")
    set(replacement_text "${search_text}")
elseif(MODE STREQUAL "regression")
    set(search_text "\"frame_ms_p95\": 0.4596814480931877")
    set(replacement_text "\"frame_ms_p95\": 1.0")
elseif(MODE STREQUAL "mismatch")
    set(search_text "\"gpu_budget_bytes\": 536870912")
    set(replacement_text "\"gpu_budget_bytes\": 536870913")
elseif(MODE STREQUAL "missing")
    set(search_text "    \"frame_ms_p95\": 0.4596814480931877,\n")
    set(replacement_text "")
else()
    message(FATAL_ERROR "unsupported MODE '${MODE}'")
endif()

string(FIND "${candidate_json}" "${search_text}" search_position)
if(search_position EQUAL -1)
    message(FATAL_ERROR "could not prepare ${MODE} qualification fixture")
endif()
string(REPLACE "${search_text}" "${replacement_text}"
       candidate_json "${candidate_json}")
set(candidate_path
    "${CMAKE_CURRENT_BINARY_DIR}/qualification-${MODE}-candidate.json")
file(WRITE "${candidate_path}" "${candidate_json}")

execute_process(
    COMMAND "${QUALIFICATION_DIFF}"
            "${BASELINE}"
            "${candidate_path}"
            "${TOLERANCES}"
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error)

if(NOT actual_exit EQUAL EXPECTED_EXIT)
    message(FATAL_ERROR
            "expected exit ${EXPECTED_EXIT}, got ${actual_exit}\nstdout:\n${standard_output}\nstderr:\n${standard_error}")
endif()
set(combined_output "${standard_output}\n${standard_error}")
string(FIND "${combined_output}" "${EXPECTED_OUTPUT}" output_position)
if(output_position EQUAL -1)
    message(FATAL_ERROR
            "expected output '${EXPECTED_OUTPUT}'\nstdout:\n${standard_output}\nstderr:\n${standard_error}")
endif()
