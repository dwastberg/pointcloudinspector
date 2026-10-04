add_library(pcinspector_app_config STATIC
        src/ApplicationConfig.cpp
        include/pci/desktop/config/ApplicationConfig.h
        src/PerformanceSettings.cpp
        include/pci/desktop/config/PerformanceSettings.h
        src/ApplicationOptions.cpp
        include/pci/desktop/config/ApplicationOptions.h
        src/MemoryBudgetPolicy.cpp
        include/pci/desktop/config/MemoryBudgetPolicy.h
        src/SupportedSource.cpp
        include/pci/desktop/config/SupportedSource.h)
add_library(pcinspector::app_config ALIAS pcinspector_app_config)
target_include_directories(pcinspector_app_config PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_app_config PUBLIC
        pcinspector::foundation
        pcinspector::platform
        pcinspector::viewport_api)
pci_configure_target(pcinspector_app_config)
