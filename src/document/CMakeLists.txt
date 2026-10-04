add_library(pcinspector_document STATIC
        include/pci/document/SceneDocument.h
        include/pci/document/SceneDocumentSnapshot.h
        include/pci/document/SceneLayer.h
        include/pci/document/SceneLayerBounds.h
        include/pci/document/SceneLayerTypes.h
        include/pci/document/SceneSnapshotLayer.h
        include/pci/document/SceneSnapshotLayerView.h
        src/SceneDocument.cpp
        src/SceneDocumentSnapshot.cpp
        src/SceneLayerBounds.cpp)
add_library(pcinspector::document ALIAS pcinspector_document)
target_include_directories(pcinspector_document PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_document PUBLIC
        pcinspector::color
        pcinspector::foundation
        pcinspector::pointcloud
        pcinspector::raster
        pcinspector::vector)
pci_configure_target(pcinspector_document)
