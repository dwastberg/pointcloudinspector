add_library(pcinspector_secure_storage STATIC
        src/ManagedStorage.cpp
        include/pci/adapters/storage/ManagedStorage.h
        src/SecureStorage.cpp
        include/pci/adapters/storage/SecureStorage.h)
add_library(pcinspector::secure_storage ALIAS pcinspector_secure_storage)
target_include_directories(pcinspector_secure_storage PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_secure_storage PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(pcinspector_secure_storage PRIVATE Advapi32)
endif()
pci_configure_target(pcinspector_secure_storage)

target_link_libraries(pcinspector_secure_storage PRIVATE pcinspector::platform)
