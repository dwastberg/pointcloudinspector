if(NOT MANIFEST OR NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR "A generated architecture MANIFEST is required")
endif()
include("${MANIFEST}")

set(violations "")
foreach(target IN LISTS PCI_ARCHITECTURE_TARGETS)
    set(observed ${PCI_OBSERVED_${target}})
    set(expected ${PCI_EXPECTED_${target}})
    if(NOT "${observed}" STREQUAL "${expected}")
        list(APPEND violations
             "${target}: expected [${expected}], observed [${observed}]")
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n  " formatted)
    message(FATAL_ERROR "Target boundary violations:\n  ${formatted}")
endif()

message(STATUS "Project target boundaries are valid")
