add_library(pcinspector_runtime_resources STATIC
        include/pci/runtime/CompletionExecutor.h
        include/pci/runtime/HierarchyResidencyCoordinator.h
        include/pci/runtime/PointMemoryBudget.h
        src/CompletionExecutor.cpp
        src/HierarchyResidencyCoordinator.cpp
        src/PointMemoryBudget.cpp)
add_library(pcinspector::runtime_resources ALIAS
        pcinspector_runtime_resources)
target_include_directories(pcinspector_runtime_resources PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_runtime_resources PUBLIC
        pcinspector::foundation)
pci_configure_target(pcinspector_runtime_resources)
