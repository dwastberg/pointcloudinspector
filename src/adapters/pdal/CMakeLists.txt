add_library(pcinspector_import_pdal STATIC
        include/pci/adapters/pdal/LocalPointIndexBuilder.h
        src/LocalPointPageFormat.cpp
        include/pci/adapters/pdal/LocalPointPageFormat.h
        src/LocalPointPageSource.cpp
        include/pci/adapters/pdal/LocalPointPageSource.h
        src/PdalHierarchicalPointSource.cpp
        include/pci/adapters/pdal/PdalHierarchicalPointSource.h
        src/LocalPageBuildInfrastructure.cpp
        include/pci/adapters/pdal/LocalPageBuildInfrastructure.h
        src/LeafPageWriter.cpp
        src/LeafPageWriter.h
        src/MortonRunBuilder.cpp
        src/MortonRunBuilder.h
        src/MortonRunRecord.h
        src/ParentHierarchyBuilder.cpp
        src/ParentHierarchyBuilder.h
        src/LocalPointIndexBuilder.cpp
        src/PdalPointMapping.cpp
        src/PdalPointMapping.h
        src/PdalPointCloudLoader.cpp
        include/pci/adapters/pdal/PdalPointCloudLoader.h
        src/PdalPointCloudStatistics.cpp
        include/pci/adapters/pdal/PdalPointCloudStatistics.h
        src/PdalSourceInspector.cpp
        include/pci/adapters/pdal/PdalSourceInspector.h)
add_library(pcinspector::import_pdal ALIAS pcinspector_import_pdal)
target_include_directories(pcinspector_import_pdal PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_import_pdal
        PUBLIC
        pcinspector::operation_api
        pcinspector::secure_storage
        PRIVATE
        Qt6::Core
        PDAL::pdalcpp
        pcinspector::platform)
pci_configure_target(pcinspector_import_pdal)
