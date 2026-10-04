# Portable raster request lifecycle and decoded-cache ownership. Dedicated tile
# workers live here; QRhi upload resources remain in the renderer target.
add_library(pcinspector_raster_runtime STATIC
        include/pci/runtime/raster/RasterTileCache.h
        include/pci/runtime/raster/RasterTileStreamer.h
        src/RasterTileCache.cpp
        src/RasterTileStreamer.cpp)
add_library(pcinspector::raster_runtime ALIAS pcinspector_raster_runtime)
target_include_directories(pcinspector_raster_runtime PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_raster_runtime PUBLIC
        pcinspector::raster)
pci_configure_target(pcinspector_raster_runtime)
