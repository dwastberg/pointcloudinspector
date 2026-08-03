function(pci_add_point_shaders target resource_name)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
                "pci_add_point_shaders: unknown target '${target}'")
    endif()

    # Single build-system inventory for the point-render and pick shader ABI.
    # The application and native-GPU tests consume identical qsb packages.
    qt_add_shaders(${target} ${resource_name}
            PREFIX "/shaders"
            BASE "${PROJECT_SOURCE_DIR}/shaders"
            FILES
            "${PROJECT_SOURCE_DIR}/shaders/edl.vert"
            "${PROJECT_SOURCE_DIR}/shaders/edl.frag"
            "${PROJECT_SOURCE_DIR}/shaders/pick.vert"
            "${PROJECT_SOURCE_DIR}/shaders/pick.frag"
            "${PROJECT_SOURCE_DIR}/shaders/points.vert"
            "${PROJECT_SOURCE_DIR}/shaders/points.frag"
            "${PROJECT_SOURCE_DIR}/shaders/vector_fill.vert"
            "${PROJECT_SOURCE_DIR}/shaders/vector_fill.frag"
            "${PROJECT_SOURCE_DIR}/shaders/vector_line.vert"
            "${PROJECT_SOURCE_DIR}/shaders/vector_line.frag"
            "${PROJECT_SOURCE_DIR}/shaders/vector_marker.vert"
            "${PROJECT_SOURCE_DIR}/shaders/vector_marker.frag")
endfunction()
