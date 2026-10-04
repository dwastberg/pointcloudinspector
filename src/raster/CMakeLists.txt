# Portable raster domain. It reaches the shared color target for ramp stops and
# must never depend on point runtime, scene, renderer, Qt, or GDAL.
add_library(pcinspector_raster STATIC
        include/pci/raster/RasterCacheKey.h
        include/pci/raster/RasterDatasetDescriptor.h
        include/pci/raster/RasterIdentity.h
        include/pci/raster/RasterLayer.h
        include/pci/raster/RasterLayerDisplay.h
        include/pci/raster/RasterPointSampler.h
        include/pci/raster/RasterRequestBatch.h
        include/pci/raster/RasterTileSource.h
        src/RasterCacheKey.cpp
        src/RasterLayer.cpp
        src/RasterLayerDisplay.cpp
        src/RasterPointSampler.cpp)
add_library(pcinspector::raster ALIAS pcinspector_raster)
target_include_directories(pcinspector_raster PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_raster PUBLIC
        pcinspector::color
        pcinspector::foundation)
pci_configure_target(pcinspector_raster)
