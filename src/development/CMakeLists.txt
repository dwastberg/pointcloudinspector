add_library(pcinspector_development_support STATIC
        src/SyntheticPointCloud.cpp
        include/pci/development/SyntheticPointCloud.h
        src/SyntheticScene.cpp
        include/pci/development/SyntheticScene.h)
add_library(pcinspector::development_support ALIAS
        pcinspector_development_support)
target_include_directories(pcinspector_development_support PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_development_support PUBLIC
        pcinspector::pointcloud
        pcinspector::point_runtime)
pci_configure_target(pcinspector_development_support)
