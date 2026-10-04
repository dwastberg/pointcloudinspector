add_library(pcinspector_navigation STATIC
        include/pci/navigation/NavigationCamera.h
        include/pci/navigation/NavigationInputState.h
        include/pci/navigation/QualificationCameraPath.h
        src/NavigationCamera.cpp
        src/NavigationInputState.cpp
        src/QualificationCameraPath.cpp)
add_library(pcinspector::navigation ALIAS pcinspector_navigation)
target_include_directories(pcinspector_navigation PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_navigation PUBLIC
        pcinspector::foundation)
pci_configure_target(pcinspector_navigation)
