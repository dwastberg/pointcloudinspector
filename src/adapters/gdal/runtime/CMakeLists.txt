# Owns GDAL registration, error capture, and the process-global block cache.
add_library(pcinspector_gdal_runtime STATIC
        src/GdalRuntime.cpp
        include/pci/adapters/gdal/runtime/GdalRuntime.h)
add_library(pcinspector::gdal_runtime ALIAS pcinspector_gdal_runtime)
target_include_directories(pcinspector_gdal_runtime PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_gdal_runtime PUBLIC
        GDAL::GDAL)
pci_configure_target(pcinspector_gdal_runtime)
