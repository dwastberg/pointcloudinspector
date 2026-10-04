include_guard(GLOBAL)

function(pci_add_header_self_containment module target)
    set(generated_directory
        "${CMAKE_CURRENT_BINARY_DIR}/header-self-containment/${module}")
    file(MAKE_DIRECTORY "${generated_directory}")

    set(translation_units "")
    foreach(header IN LISTS ARGN)
        string(REPLACE "/" "_" source_name "${header}")
        string(REPLACE "." "_" source_name "${source_name}")
        set(translation_unit "${generated_directory}/${source_name}.cpp")
        file(GENERATE OUTPUT "${translation_unit}" CONTENT "#include <${header}>\n")
        list(APPEND translation_units "${translation_unit}")
    endforeach()

    set(check_target "pcinspector_${module}_header_self_containment")
    add_library("${check_target}" OBJECT ${translation_units})
    if(NOT PCINSPECTOR_CHECK_HEADERS)
        set_target_properties("${check_target}" PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
    target_link_libraries("${check_target}" PRIVATE "${target}")
    pci_configure_target("${check_target}")
    set_property(GLOBAL APPEND PROPERTY
        PCI_HEADER_SELF_CONTAINMENT_TARGETS "${check_target}")
endfunction()

function(pci_add_header_self_containment_target)
    get_property(check_targets GLOBAL PROPERTY
        PCI_HEADER_SELF_CONTAINMENT_TARGETS)
    add_custom_target(header-self-containment DEPENDS ${check_targets})
endfunction()
