add_library(pcinspector_desktop_session STATIC
        src/PointSceneCoordinator.cpp
        include/pci/desktop/session/PointSceneCoordinator.h
        include/pci/desktop/session/DocumentUpdate.h
        src/SceneOperationBindings.cpp
        src/ScenePublicationCoordinator.cpp
        src/SceneSession.cpp
        include/pci/desktop/session/SceneSession.h)
add_library(pcinspector::desktop_session ALIAS pcinspector_desktop_session)
target_include_directories(pcinspector_desktop_session PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
target_link_libraries(pcinspector_desktop_session PUBLIC
        pcinspector::app_config
        pcinspector::app_model
        pcinspector::color
        pcinspector::document
        pcinspector::desktop_operations
        pcinspector::scene_runtime
        Qt6::Core
        PRIVATE
        pcinspector::platform
        pcinspector::raster)
set_target_properties(pcinspector_desktop_session PROPERTIES AUTOMOC ON)
pci_configure_target(pcinspector_desktop_session)
