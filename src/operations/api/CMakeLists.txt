add_library(pcinspector_operation_api STATIC
        include/pci/operations/StorageMaintenance.h
        include/pci/operations/LoadJobKey.h
        include/pci/operations/LoadJobMechanics.h
        include/pci/operations/PointCloudImport.h
        include/pci/operations/PointCloudLoader.h
        include/pci/operations/PreparedPointDataset.h
        include/pci/operations/PointCloudStatistics.h
        include/pci/operations/RasterImport.h
        include/pci/operations/RasterColorizeRunStore.h
        include/pci/operations/RasterPointColorize.h
        include/pci/operations/VectorImport.h
        src/PointCloudLoader.cpp
        src/PreparedPointDataset.cpp
        src/RasterColorizeRunStore.cpp
        src/RasterPointColorize.cpp)
add_library(pcinspector::operation_api ALIAS pcinspector_operation_api)
target_include_directories(pcinspector_operation_api PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_operation_api PUBLIC
        pcinspector::foundation
        pcinspector::pointcloud
        pcinspector::raster
        pcinspector::runtime_resources
        pcinspector::vector)
pci_configure_target(pcinspector_operation_api)
