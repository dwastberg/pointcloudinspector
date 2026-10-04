add_library(pcinspector_renderer STATIC
        src/FrameRenderer.cpp
        include/pci/rendering/rhi/FrameRenderer.h
        src/PointColorMapAtlas.cpp
        include/pci/rendering/rhi/PointColorMapAtlas.h
        src/EyeDomeLightingPass.cpp
        include/pci/rendering/rhi/EyeDomeLightingPass.h
        src/PointCloudRenderer.cpp
        include/pci/rendering/rhi/PointCloudRenderer.h
        src/PointPicker.cpp
        include/pci/rendering/rhi/PointPicker.h
        src/RasterLayerRenderer.cpp
        include/pci/rendering/rhi/RasterLayerRenderer.h
        include/pci/rendering/rhi/RhiResource.h
        src/ShaderLoader.cpp
        include/pci/rendering/rhi/ShaderLoader.h
        src/UniformStaging.cpp
        include/pci/rendering/rhi/UniformStaging.h
        src/VectorLayerRenderer.cpp
        include/pci/rendering/rhi/VectorLayerRenderer.h
        src/UploadScheduler.cpp
        include/pci/rendering/rhi/UploadScheduler.h)
add_library(pcinspector::renderer ALIAS pcinspector_renderer)
target_include_directories(pcinspector_renderer PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_renderer
        PUBLIC
        pcinspector::color
        Qt6::Gui
        PRIVATE
        pcinspector::navigation
        pcinspector::platform
        pcinspector::pointcloud
        pcinspector::document
        pcinspector::raster
        pcinspector::raster_runtime
        pcinspector::render_telemetry
        pcinspector::frame
        pcinspector::renderer_planning
        pcinspector::scene_runtime
        pcinspector::vector
        Qt6::GuiPrivate)
pci_configure_target(pcinspector_renderer)

target_link_libraries(pcinspector_renderer PUBLIC Qt6::GuiPrivate)

target_link_libraries(pcinspector_renderer PUBLIC pcinspector::frame)
