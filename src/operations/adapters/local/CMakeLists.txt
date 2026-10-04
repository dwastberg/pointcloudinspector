add_library(pcinspector_operation_local STATIC
        include/pci/operations/local/LocalRasterColorizeRunStore.h
        src/LocalRasterColorizeRunStore.cpp
        src/LocalStorageMaintenance.cpp
        include/pci/operations/local/LocalStorageMaintenance.h)
add_library(pcinspector::operation_local ALIAS pcinspector_operation_local)
target_include_directories(pcinspector_operation_local PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_operation_local
        PUBLIC pcinspector::operation_api
        PRIVATE
        pcinspector::platform
        pcinspector::secure_storage
        Qt6::Core)
pci_configure_target(pcinspector_operation_local)
