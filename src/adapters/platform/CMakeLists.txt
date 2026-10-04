add_library(pcinspector_platform STATIC
        src/ProcessMemory.cpp
        include/pci/adapters/platform/ProcessMemory.h
        src/SystemMemoryInfo.cpp
        include/pci/adapters/platform/SystemMemoryInfo.h)
add_library(pcinspector::platform ALIAS pcinspector_platform)
target_include_directories(pcinspector_platform PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_platform PRIVATE
        pcinspector::foundation)
pci_configure_target(pcinspector_platform)
if(WIN32)
    target_link_libraries(pcinspector_platform PRIVATE Psapi)
endif()

target_sources(pcinspector_platform PRIVATE include/pci/adapters/platform/QtPath.h)
target_link_libraries(pcinspector_platform PUBLIC Qt6::Core)
