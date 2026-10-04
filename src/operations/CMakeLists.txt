add_library(pcinspector_operations STATIC
        import/src/RasterColorizeOperation.cpp
        import/src/RasterElevationOperation.cpp
        import/src/RasterImportOperation.cpp
        import/src/VectorImportOperation.cpp
        import/src/PointImportOperation.cpp
        jobs/src/OperationRegistry.cpp
        jobs/src/StatisticsOperation.cpp
        jobs/src/StorageMaintenanceOperation.cpp
        jobs/include/pci/operations/StorageMaintenanceOperation.h
        colorize/include/pci/operations/RasterPointColorizer.h
        colorize/src/RasterPointColorizer.cpp
        colorize/include/pci/operations/PointDatasetIngestion.h
        colorize/src/PointDatasetIngestion.cpp
        colorize/include/pci/operations/PointDatasetInstallation.h
        colorize/src/PointDatasetInstallation.cpp)
add_library(pcinspector::operations ALIAS pcinspector_operations)
target_include_directories(pcinspector_operations PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/colorize/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/jobs/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/import/include")
target_link_libraries(pcinspector_operations
        PUBLIC
        pcinspector::operation_api
        pcinspector::point_runtime
        PRIVATE
        pcinspector::tasking)
pci_configure_target(pcinspector_operations)
