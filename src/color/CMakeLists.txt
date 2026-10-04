add_library(pcinspector_color STATIC
        include/pci/color/ColorRamp.h
        include/pci/color/CptColorMapParser.h
        include/pci/color/PointColorMapCatalog.h
        include/pci/color/PointColorTypes.h
        src/ColorRamp.cpp
        src/CptColorMapParser.cpp
        src/PointColorMapCatalog.cpp)
add_library(pcinspector::color ALIAS pcinspector_color)
target_include_directories(pcinspector_color PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
pci_configure_target(pcinspector_color)
