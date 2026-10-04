add_library(pcinspector_point_runtime STATIC
        include/pci/runtime/point/DecodedCacheMetrics.h
        include/pci/runtime/point/DecodedPageCache.h
        include/pci/runtime/point/PointColorInstallation.h
        include/pci/runtime/point/PointDatasetRuntime.h
        include/pci/runtime/point/PointDecodeQueue.h
        include/pci/runtime/point/PointResidencyService.h
        include/pci/runtime/point/RasterColorizedPointSource.h
        src/DecodedPageCache.cpp
        src/PointDatasetRuntime.cpp
        src/PointResidencyService.cpp
        src/RasterColorizedPointSource.cpp)
add_library(pcinspector::point_runtime ALIAS pcinspector_point_runtime)
target_include_directories(pcinspector_point_runtime PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_point_runtime PUBLIC
        pcinspector::foundation
        pcinspector::pointcloud
        pcinspector::runtime_resources
        pcinspector::tasking)
pci_configure_target(pcinspector_point_runtime)
