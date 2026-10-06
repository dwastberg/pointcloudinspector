# Exercise the same libuv child-process handling used by configure and CTest.
# An external timeout is intentional: a CMake execute_process TIMEOUT can wake
# the event loop and hide the Rosetta lost-SIGCHLD failure this checks for.
message(STATUS "Checking CMake child-process handling (architecture: $ENV{PCI_CONTAINER_ARCH})")
foreach(iteration RANGE 1 1000)
    execute_process(COMMAND /bin/sh -c "echo child"
        OUTPUT_VARIABLE child_output RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Child process ${iteration} failed: ${result}")
    endif()
endforeach()
message(STATUS "CMake child-process check passed")
