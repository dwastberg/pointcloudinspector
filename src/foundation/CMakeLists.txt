add_library(pcinspector_foundation STATIC
        include/pci/foundation/Bounds3d.h
        include/pci/foundation/CheckedArithmetic.h
        include/pci/foundation/Generation.h
        include/pci/foundation/Hash.h
        include/pci/foundation/JobResult.h
        include/pci/foundation/LayerIdentity.h
        include/pci/foundation/SpatialReferenceComparator.h
        include/pci/foundation/StrongId.h
        include/pci/foundation/Vec3d.h
        src/Bounds3d.cpp)
add_library(pcinspector::foundation ALIAS pcinspector_foundation)
target_include_directories(pcinspector_foundation PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
pci_configure_target(pcinspector_foundation)
