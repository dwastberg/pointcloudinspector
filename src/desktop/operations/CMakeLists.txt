add_library(pcinspector_desktop_operations STATIC
        src/LoadJobRow.cpp
        include/pci/desktop/operations/LoadJobRow.h
        src/PointCloudColorizeController.cpp
        include/pci/desktop/operations/PointCloudColorizeController.h
        src/PointCloudLoadController.cpp
        include/pci/desktop/operations/PointCloudLoadController.h
        src/RasterElevationController.cpp
        include/pci/desktop/operations/RasterElevationController.h
        src/RasterLoadController.cpp
        include/pci/desktop/operations/RasterLoadController.h
        src/VectorLoadController.cpp
        include/pci/desktop/operations/VectorLoadController.h
        include/pci/desktop/operations/ImportServices.h)
add_library(pcinspector::desktop_operations ALIAS pcinspector_desktop_operations)
target_include_directories(pcinspector_desktop_operations PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_desktop_operations
        PUBLIC
        pcinspector::desktop_dispatch
        pcinspector::document
        pcinspector::foundation
        pcinspector::operation_api
        pcinspector::operations
        pcinspector::raster
        pcinspector::tasking
        Qt6::Core
        PRIVATE
        pcinspector::platform
        pcinspector::runtime_resources)
set_target_properties(pcinspector_desktop_operations PROPERTIES AUTOMOC ON)
pci_configure_target(pcinspector_desktop_operations)
