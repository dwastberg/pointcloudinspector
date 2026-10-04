add_library(pcinspector_desktop_dispatch STATIC
        src/QtCompletionExecutor.cpp
        include/pci/desktop/dispatch/QtCompletionExecutor.h)
add_library(pcinspector::desktop_dispatch ALIAS pcinspector_desktop_dispatch)
target_include_directories(pcinspector_desktop_dispatch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_desktop_dispatch PUBLIC pcinspector::runtime_resources Qt6::Core)
set_target_properties(pcinspector_desktop_dispatch PROPERTIES AUTOMOC ON)
pci_configure_target(pcinspector_desktop_dispatch)
