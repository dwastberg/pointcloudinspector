add_library(pcinspector_earcut INTERFACE)
add_library(pcinspector::earcut ALIAS pcinspector_earcut)
target_include_directories(pcinspector_earcut SYSTEM INTERFACE
        "${PROJECT_SOURCE_DIR}/third_party/earcut/include")

add_library(pcinspector_vector STATIC
        include/pci/vector/VectorGeometry.h
        include/pci/vector/VectorLayerData.h
        include/pci/vector/VectorLayerStyle.h
        src/VectorGeometry.cpp
        src/VectorLayerData.cpp
        src/VectorLayerStyle.cpp)
add_library(pcinspector::vector ALIAS pcinspector_vector)
target_include_directories(pcinspector_vector PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_vector
        PUBLIC pcinspector::foundation
        PRIVATE pcinspector::earcut)
pci_configure_target(pcinspector_vector)
