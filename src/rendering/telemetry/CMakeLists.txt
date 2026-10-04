add_library(pcinspector_render_telemetry STATIC
        include/pci/rendering/RenderTelemetry.h
        src/RenderTelemetry.cpp)
add_library(pcinspector::render_telemetry ALIAS
        pcinspector_render_telemetry)
target_include_directories(pcinspector_render_telemetry PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_render_telemetry PUBLIC
        pcinspector::foundation)
pci_configure_target(pcinspector_render_telemetry)
