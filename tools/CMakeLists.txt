# Decode-only profiling helper. Build in Release for meaningful measurements.
add_executable(pci_load_bench EXCLUDE_FROM_ALL pci_load_bench.cpp)
target_link_libraries(pci_load_bench PRIVATE
        pcinspector::app_config
        pcinspector::import_pdal pcinspector::operations
        pcinspector::platform)
set_target_properties(pci_load_bench PROPERTIES FOLDER Tools)
pci_configure_target(pci_load_bench)

# Local hierarchy cache/decode qualification helper. Unlike pci_load_bench,
# this repeatedly revisits a working set larger than the decoded cache.
add_executable(pci_residency_bench pci_residency_bench.cpp)
target_link_libraries(pci_residency_bench PRIVATE
        pcinspector::app_config
        pcinspector::document
        pcinspector::import_pdal pcinspector::operations
        pcinspector::platform
        pcinspector::scene_runtime)
set_target_properties(pci_residency_bench PROPERTIES FOLDER Tools)
pci_configure_target(pci_residency_bench)

# End-to-end local multi-file qualification. This uses the production import
# controller and one shared document, and can archive machine-readable data.
add_executable(pci_multifile_bench pci_multifile_bench.cpp)
target_link_libraries(pci_multifile_bench PRIVATE
        pcinspector::app_config
        pcinspector::document
        pcinspector::desktop_operations
        pcinspector::import_pdal pcinspector::operations
        pcinspector::platform
        pcinspector::scene_runtime)
set_target_properties(pci_multifile_bench PROPERTIES FOLDER Tools)
pci_configure_target(pci_multifile_bench)

# Named, non-default release acceptance for raster-to-point colorization. The
# default invocation processes 22.9M stored hierarchy samples.
add_executable(pci_raster_colorize_bench EXCLUDE_FROM_ALL pci_raster_colorize_bench.cpp)
target_link_libraries(pci_raster_colorize_bench PRIVATE
        pcinspector::operation_local
        pcinspector::operations)
set_target_properties(pci_raster_colorize_bench PROPERTIES FOLDER Tools)
pci_configure_target(pci_raster_colorize_bench)

# Schema-aware performance qualification comparison. This stays independent of
# the application libraries so it can validate reports without a GPU runtime.
add_executable(pci_qualification_diff
        pci_qualification_diff.cpp
        qualification/QualificationDiff.cpp)
target_include_directories(pci_qualification_diff PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(pci_qualification_diff PRIVATE pcinspector::platform Qt6::Core)
set_target_properties(pci_qualification_diff PROPERTIES FOLDER Tools)
pci_configure_target(pci_qualification_diff)
