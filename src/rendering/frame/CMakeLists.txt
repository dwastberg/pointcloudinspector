add_library(pcinspector_frame STATIC
        src/FramePickGuard.cpp
        include/pci/rendering/FramePickGuard.h
        src/SceneSnapshotCache.cpp
        include/pci/rendering/SceneSnapshotCache.h
        src/FrameExecutor.cpp
        include/pci/rendering/FrameExecutor.h
        src/PointFrameExecutor.cpp
        include/pci/rendering/PointFrameExecutor.h)
add_library(pcinspector::frame ALIAS
        pcinspector_frame)
target_include_directories(pcinspector_frame PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_frame PUBLIC
        pcinspector::render_telemetry
        pcinspector::foundation
        pcinspector::renderer_planning
        pcinspector::document
        pcinspector::raster_runtime
        pcinspector::scene_runtime)
pci_configure_target(pcinspector_frame)
