add_library(pcinspector_app_model STATIC
        src/LoadingProgressModel.cpp
        include/pci/desktop/models/LoadingProgressModel.h)
add_library(pcinspector::app_model ALIAS pcinspector_app_model)
target_include_directories(pcinspector_app_model PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_app_model PUBLIC
        pcinspector::operation_api
        pcinspector::viewport_api)
pci_configure_target(pcinspector_app_model)
