add_library(pcinspector_renderer_planning STATIC
        src/AdaptivePointBudget.cpp
        include/pci/rendering/planning/AdaptivePointBudget.h
        src/FrameCamera.cpp
        include/pci/rendering/planning/FrameCamera.h
        src/FramePlanner.cpp
        include/pci/rendering/planning/FramePlanner.h
        src/FrustumCuller.cpp
        include/pci/rendering/planning/FrustumCuller.h
        src/Measurement.cpp
        include/pci/rendering/planning/Measurement.h
        src/MeasurementController.cpp
        include/pci/rendering/planning/MeasurementController.h
        src/PointCloudSourceBoundsIndex.cpp
        include/pci/rendering/planning/PointCloudSourceBoundsIndex.h
        src/PointFrameCoordinator.cpp
        include/pci/rendering/planning/PointFrameCoordinator.h
        src/PointSizePolicy.cpp
        include/pci/rendering/planning/PointSizePolicy.h
        src/RasterFrameCoordinator.cpp
        include/pci/rendering/planning/RasterFrameCoordinator.h
        src/RasterLodPlanner.cpp
        include/pci/rendering/planning/RasterLodPlanner.h
        include/pci/rendering/planning/RenderActivity.h
        src/RenderSelection.cpp
        include/pci/rendering/planning/RenderSelection.h
        src/SceneVisibilityIndex.cpp
        include/pci/rendering/planning/SceneVisibilityIndex.h)
add_library(pcinspector::renderer_planning ALIAS
        pcinspector_renderer_planning)
target_include_directories(pcinspector_renderer_planning PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_renderer_planning PUBLIC
        pcinspector::foundation
        pcinspector::navigation)
pci_configure_target(pcinspector_renderer_planning)

target_link_libraries(pcinspector_renderer_planning PUBLIC
        pcinspector::pointcloud
        pcinspector::raster)

target_link_libraries(pcinspector_renderer_planning PUBLIC pcinspector::document)
