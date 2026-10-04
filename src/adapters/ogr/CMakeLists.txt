add_library(pcinspector_import_ogr STATIC
        src/OgrVectorLoader.cpp
        include/pci/adapters/ogr/OgrVectorLoader.h)
add_library(pcinspector::import_ogr ALIAS pcinspector_import_ogr)
target_include_directories(pcinspector_import_ogr PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_import_ogr
        PUBLIC
        pcinspector::operation_api
        pcinspector::vector
        PRIVATE
        pcinspector::gdal_runtime)
pci_configure_target(pcinspector_import_ogr)
