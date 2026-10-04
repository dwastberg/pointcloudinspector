include_guard(GLOBAL)

# Register composition once; header checks and architecture checks consume it.
# Dependency policies remain independently declared in PciArchitecture.cmake.
function(pci_add_module directory target)
    add_subdirectory("${directory}")
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Module '${directory}' did not define '${target}'")
    endif()
    set_property(GLOBAL APPEND PROPERTY PCI_MODULE_TARGETS "${target}")
endfunction()
