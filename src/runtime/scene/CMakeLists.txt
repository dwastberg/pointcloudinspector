add_library(pcinspector_scene_runtime STATIC
        include/pci/runtime/scene/RuntimeBudgetSnapshot.h
        include/pci/runtime/scene/SceneRuntime.h
        src/SceneRuntime.cpp)
add_library(pcinspector::scene_runtime ALIAS pcinspector_scene_runtime)
target_include_directories(pcinspector_scene_runtime PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcinspector_scene_runtime PUBLIC
        pcinspector::foundation
        pcinspector::pointcloud
        pcinspector::point_runtime
        pcinspector::raster
        pcinspector::runtime_resources
        pcinspector::tasking)
pci_configure_target(pcinspector_scene_runtime)
