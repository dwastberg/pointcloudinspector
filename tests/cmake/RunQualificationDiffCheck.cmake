foreach(required_variable IN ITEMS
        QUALIFICATION_DIFF
        BASELINE
        TOLERANCES
        MODE
        OUTPUT_DIR
        EXPECTED_EXIT
        EXPECTED_OUTPUT)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

foreach(required_file IN ITEMS QUALIFICATION_DIFF BASELINE TOLERANCES)
    if(NOT EXISTS "${${required_file}}")
        message(FATAL_ERROR
                "${required_file} does not exist: '${${required_file}}'")
    endif()
endforeach()

file(READ "${BASELINE}" baseline_json)
file(READ "${TOLERANCES}" tolerances_json)
string(JSON baseline_type TYPE "${baseline_json}")
string(JSON tolerances_type TYPE "${tolerances_json}")
if(NOT baseline_type STREQUAL "OBJECT" OR
        NOT tolerances_type STREQUAL "OBJECT")
    message(FATAL_ERROR "qualification fixtures must contain JSON objects")
endif()
set(candidate_json "${baseline_json}")

function(require_member_type json field expected_type)
    string(JSON actual_type TYPE "${json}" "${field}")
    if(NOT actual_type STREQUAL expected_type)
        message(FATAL_ERROR
                "field '${field}' must have JSON type ${expected_type}, got ${actual_type}")
    endif()
endfunction()

if(MODE STREQUAL "ignored")
    require_member_type("${candidate_json}" timestamp_utc STRING)
    require_member_type("${candidate_json}" layers ARRAY)
    string(JSON candidate_json SET "${candidate_json}"
           timestamp_utc "\"2099-01-01T00:00:00.000Z\"")
    string(JSON layer_count LENGTH "${candidate_json}" layers)
    if(layer_count LESS 1)
        message(FATAL_ERROR "ignored-value fixture requires at least one layer")
    endif()
    math(EXPR last_layer "${layer_count} - 1")
    foreach(layer_index RANGE 0 ${last_layer})
        string(JSON layer_type TYPE "${candidate_json}" layers ${layer_index})
        string(JSON path_type TYPE "${candidate_json}"
               layers ${layer_index} path)
        if(NOT layer_type STREQUAL "OBJECT" OR NOT path_type STREQUAL "STRING")
            message(FATAL_ERROR
                    "layers[${layer_index}].path must be a JSON string")
        endif()
        set(new_path "synthetic/ignored-source-${layer_index}.laz")
        string(JSON candidate_json SET "${candidate_json}"
               layers ${layer_index} path "\"${new_path}\"")
        string(JSON written_path GET "${candidate_json}"
               layers ${layer_index} path)
        if(NOT written_path STREQUAL new_path)
            message(FATAL_ERROR
                    "failed to mutate layers[${layer_index}].path")
        endif()
    endforeach()
    string(JSON written_timestamp GET "${candidate_json}" timestamp_utc)
    if(NOT written_timestamp STREQUAL "2099-01-01T00:00:00.000Z")
        message(FATAL_ERROR "failed to mutate timestamp_utc")
    endif()
    set(allowed_change timestamp_utc)
elseif(MODE STREQUAL "regression")
    require_member_type("${candidate_json}" frame_ms_p95 NUMBER)
    string(JSON baseline_p95 GET "${candidate_json}" frame_ms_p95)
    if(NOT baseline_p95 MATCHES "^1([.]0+)?([eE][+-]?0+)?$")
        message(FATAL_ERROR
                "regression fixture frame_ms_p95 must be numerically 1, got '${baseline_p95}'")
    endif()
    string(JSON tolerance_type TYPE "${tolerances_json}"
           measurements sampled_frame_p95 maximum_relative_increase)
    string(JSON p95_tolerance GET "${tolerances_json}"
           measurements sampled_frame_p95 maximum_relative_increase)
    if(NOT tolerance_type STREQUAL "NUMBER" OR
            p95_tolerance LESS 0.099999 OR
            p95_tolerance GREATER 0.100001)
        message(FATAL_ERROR
                "sampled_frame_p95 tolerance must be numerically 0.10, got '${p95_tolerance}'")
    endif()
    string(JSON candidate_json SET "${candidate_json}" frame_ms_p95 2.0)
    set(allowed_change frame_ms_p95)
elseif(MODE STREQUAL "mismatch")
    require_member_type("${candidate_json}" gpu_budget_bytes NUMBER)
    string(JSON gpu_budget GET "${candidate_json}" gpu_budget_bytes)
    if(NOT gpu_budget MATCHES "^[0-9]+$")
        message(FATAL_ERROR
                "gpu_budget_bytes must be an integer for this mutation, got '${gpu_budget}'")
    endif()
    math(EXPR changed_gpu_budget "${gpu_budget} + 1")
    string(JSON candidate_json SET "${candidate_json}"
           gpu_budget_bytes ${changed_gpu_budget})
    set(allowed_change gpu_budget_bytes)
elseif(MODE STREQUAL "missing")
    require_member_type("${candidate_json}" frame_ms_p95 NUMBER)
    string(JSON candidate_json REMOVE "${candidate_json}" frame_ms_p95)
    set(allowed_change frame_ms_p95)
else()
    message(FATAL_ERROR "unsupported MODE '${MODE}'")
endif()

set(monitored_fields
    schema
    status
    selected_backend
    gpu_validation
    cpu_budget_bytes
    gpu_budget_bytes
    source_count
    source_file_bytes
    source_points
    first_any_preview_ms
    first_all_preview_ms
    display_ready_ms
    frame_ms_p95
    frame_ms_max
    cpu_peak_bytes
    gpu_peak_bytes
    draw_calls
    cache_evictions)
foreach(field IN LISTS monitored_fields)
    if(field STREQUAL allowed_change)
        continue()
    endif()
    string(JSON before GET "${baseline_json}" "${field}")
    string(JSON after GET "${candidate_json}" "${field}")
    if(NOT before STREQUAL after)
        message(FATAL_ERROR
                "${MODE} mutation unexpectedly changed '${field}'")
    endif()
endforeach()

if(MODE STREQUAL "missing")
    string(JSON removed_type ERROR_VARIABLE removed_error
           TYPE "${candidate_json}" frame_ms_p95)
    if(removed_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "failed to remove frame_ms_p95")
    endif()
elseif(MODE STREQUAL "regression")
    string(JSON changed_p95 GET "${candidate_json}" frame_ms_p95)
    if(NOT changed_p95 MATCHES "^2([.]0+)?([eE][+-]?0+)?$")
        message(FATAL_ERROR "failed to set frame_ms_p95 to 2.0")
    endif()
elseif(MODE STREQUAL "mismatch")
    string(JSON written_budget GET "${candidate_json}" gpu_budget_bytes)
    if(NOT written_budget STREQUAL "${changed_gpu_budget}")
        message(FATAL_ERROR "failed to increment gpu_budget_bytes")
    endif()
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(candidate_path "${OUTPUT_DIR}/qualification-${MODE}-candidate.json")
file(WRITE "${candidate_path}" "${candidate_json}\n")
file(READ "${candidate_path}" written_candidate)
string(JSON written_type TYPE "${written_candidate}")
if(NOT written_type STREQUAL "OBJECT")
    message(FATAL_ERROR "generated candidate is not a JSON object")
endif()

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
