add_library(pcinspector_import_gdal STATIC
        src/GdalRasterDataset.cpp
        include/pci/adapters/gdal/GdalRasterDataset.h
        src/GdalRasterLoader.cpp
        include/pci/adapters/gdal/GdalRasterLoader.h
        src/GdalSpatialReferenceComparator.cpp
        include/pci/adapters/gdal/GdalSpatialReferenceComparator.h
        src/GdalRasterSource.cpp
        include/pci/adapters/gdal/GdalRasterSource.h)
add_library(pcinspector::import_gdal ALIAS pcinspector_import_gdal)
target_include_directories(pcinspector_import_gdal PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_import_gdal
        PUBLIC
        pcinspector::operation_api
        pcinspector::raster
        PRIVATE
        pcinspector::color
        pcinspector::gdal_runtime)
pci_configure_target(pcinspector_import_gdal)

target_link_libraries(pcinspector_import_gdal PUBLIC pcinspector::gdal_runtime)
