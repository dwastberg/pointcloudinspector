add_library(pcinspector_viewport STATIC
        src/BackendPolicy.cpp
        include/pci/desktop/viewport/BackendPolicy.h
        src/ViewportInputController.cpp
        include/pci/desktop/viewport/ViewportInputController.h
        src/RenderViewportWidget.cpp
        include/pci/desktop/viewport/RenderViewportWidget_p.h
        src/RenderMetricsProjection.cpp
        include/pci/desktop/viewport/RenderMetricsProjection.h)
add_library(pcinspector::viewport ALIAS pcinspector_viewport)
target_include_directories(pcinspector_viewport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_viewport PUBLIC
        pcinspector::viewport_api pcinspector::renderer pcinspector::frame
        pcinspector::render_telemetry Qt6::Widgets
        PRIVATE pcinspector::desktop_dispatch pcinspector::platform Qt6::GuiPrivate)
pci_configure_target(pcinspector_viewport)

target_link_libraries(pcinspector_viewport PUBLIC Qt6::GuiPrivate)
