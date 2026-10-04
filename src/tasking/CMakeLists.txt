add_library(pcinspector_tasking STATIC
        include/pci/tasking/TaskScheduler.h
        src/TaskScheduler.cpp)
add_library(pcinspector::tasking ALIAS pcinspector_tasking)
target_include_directories(pcinspector_tasking PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_tasking PUBLIC
        pcinspector::foundation)
pci_configure_target(pcinspector_tasking)
