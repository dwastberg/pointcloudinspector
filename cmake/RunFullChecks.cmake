# Run the complete local static-analysis and sanitizer suite from the repository
# root with:
#
#   cmake -P cmake/RunFullChecks.cmake
#
# The default build parallelism is deliberately conservative. Override it with:
#
#   cmake -DPCINSPECTOR_LOCAL_CHECK_JOBS=6 -P cmake/RunFullChecks.cmake
#
# ThreadSanitizer is part of the full check. On a toolchain that cannot run it,
# the other checks can still be requested explicitly with:
#
#   cmake -DPCINSPECTOR_LOCAL_CHECK_TSAN=OFF -P cmake/RunFullChecks.cmake

cmake_minimum_required(VERSION 3.24)

get_filename_component(pcinspector_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(cmake_program_directory "${CMAKE_COMMAND}" DIRECTORY)

find_program(
    ctest_program
    NAMES ctest ctest.exe
    HINTS "${cmake_program_directory}"
    REQUIRED)

if(NOT DEFINED PCINSPECTOR_LOCAL_CHECK_JOBS)
    set(PCINSPECTOR_LOCAL_CHECK_JOBS 3)
endif()
if(NOT PCINSPECTOR_LOCAL_CHECK_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
            "PCINSPECTOR_LOCAL_CHECK_JOBS must be a positive integer")
endif()

if(NOT DEFINED PCINSPECTOR_LOCAL_CHECK_TSAN)
    set(PCINSPECTOR_LOCAL_CHECK_TSAN ON)
endif()

# Reuse existing builds unless a full rebuild is explicitly requested.
if(NOT DEFINED PCINSPECTOR_LOCAL_CHECK_CLEAN)
    set(PCINSPECTOR_LOCAL_CHECK_CLEAN OFF)
endif()
if(NOT DEFINED PCINSPECTOR_LOCAL_CHECK_LONG_STRESS)
    set(PCINSPECTOR_LOCAL_CHECK_LONG_STRESS OFF)
endif()

function(pcinspector_run_step description)
    message(STATUS "${description}")
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${pcinspector_root}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${description} failed with exit code ${result}")
    endif()
endfunction()

function(pcinspector_configure_and_build preset)
    set(configure_options)
    set(build_options)
    if(PCINSPECTOR_LOCAL_CHECK_CLEAN)
        list(APPEND configure_options --fresh)
        list(APPEND build_options --clean-first)
    endif()
    pcinspector_run_step(
        "Configuring ${preset}"
        "${CMAKE_COMMAND}" ${configure_options} --preset "${preset}"
        "-DPCINSPECTOR_ENABLE_LONG_STRESS_TESTS=${PCINSPECTOR_LOCAL_CHECK_LONG_STRESS}")
    pcinspector_run_step(
        "Building ${preset}"
        "${CMAKE_COMMAND}" --build --preset "${preset}" ${build_options}
        --parallel "${PCINSPECTOR_LOCAL_CHECK_JOBS}")
endfunction()

message(STATUS "Point Cloud Inspector full local checks")
message(STATUS "Source: ${pcinspector_root}")
message(STATUS "Build jobs: ${PCINSPECTOR_LOCAL_CHECK_JOBS}")

pcinspector_configure_and_build(clang-tidy)
pcinspector_run_step(
    "Checking source formatting"
    "${CMAKE_COMMAND}" --build --preset clang-tidy --target format-check)

pcinspector_configure_and_build(asan-ubsan)
pcinspector_run_step(
    "Running tests with AddressSanitizer and UndefinedBehaviorSanitizer"
    "${ctest_program}" --preset asan-ubsan)

if(PCINSPECTOR_LOCAL_CHECK_TSAN)
    pcinspector_configure_and_build(tsan)
    pcinspector_run_step(
        "Running tests with ThreadSanitizer"
        "${ctest_program}" --preset tsan)
else()
    message(STATUS "Skipping ThreadSanitizer by explicit request")
endif()

message(STATUS "All requested static-analysis and sanitizer checks passed")
