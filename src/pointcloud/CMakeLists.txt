add_library(pcinspector_pointcloud STATIC
        include/pci/pointcloud/PointDatasetRuntimeSnapshot.h
        include/pci/pointcloud/BlockPartitioner.h
        include/pci/pointcloud/GpuPoint.h
        include/pci/pointcloud/GpuPointProperties.h
        include/pci/pointcloud/PointAttributes.h
        include/pci/pointcloud/PointBlock.h
        include/pci/pointcloud/PointClassificationFilter.h
        include/pci/pointcloud/PointCloudDataSource.h
        include/pci/pointcloud/PointCloudMetadata.h
        include/pci/pointcloud/PointCloudNode.h
        include/pci/pointcloud/PointDatasetDescriptor.h
        include/pci/pointcloud/PointColorPolicy.h
        include/pci/pointcloud/PointIdentity.h
        include/pci/pointcloud/PointResidencyView.h
        include/pci/pointcloud/PointSample.h
        include/pci/pointcloud/RasterPointColorData.h
        include/pci/pointcloud/SceneBlock.h
        include/pci/pointcloud/SourcePoint.h
        src/BlockPartitioner.cpp
        src/PointBlock.cpp
        src/PointCloudNode.cpp
        src/PointIdentity.cpp
        src/PointColorPolicy.cpp)
add_library(pcinspector::pointcloud ALIAS pcinspector_pointcloud)
target_include_directories(pcinspector_pointcloud PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_pointcloud PUBLIC
        pcinspector::color
        pcinspector::foundation)
pci_configure_target(pcinspector_pointcloud)
