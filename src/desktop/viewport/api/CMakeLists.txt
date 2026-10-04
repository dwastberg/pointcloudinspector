add_library(pcinspector_viewport_api INTERFACE)
add_library(pcinspector::viewport_api ALIAS pcinspector_viewport_api)
target_sources(pcinspector_viewport_api INTERFACE
        include/pci/desktop/viewport/GraphicsApi.h
        include/pci/desktop/viewport/PointColorLabels.h
        include/pci/desktop/viewport/RenderLoadProgress.h
        include/pci/desktop/viewport/RenderMetrics.h
        include/pci/desktop/viewport/RenderViewport.h)
target_include_directories(pcinspector_viewport_api INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_viewport_api INTERFACE
        pcinspector::color
        pcinspector::document
        pcinspector::project_options
        pcinspector::scene_runtime
        Qt6::Core)

target_link_libraries(pcinspector_viewport_api INTERFACE pcinspector::renderer_planning)
