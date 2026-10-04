add_subdirectory(foundation)
add_subdirectory(tasking)
add_subdirectory(runtime_resources)
add_subdirectory(color)
add_subdirectory(pointcloud)
add_subdirectory(raster)
add_subdirectory(raster_runtime)
add_subdirectory(vector)
add_subdirectory(document)
add_subdirectory(operations/api)
add_subdirectory(adapters/platform)
add_subdirectory(adapters/storage)
add_subdirectory(operations/adapters/local)
add_subdirectory(navigation)
add_subdirectory(rendering/telemetry)
add_subdirectory(rendering/planning)
add_subdirectory(runtime/point)
add_subdirectory(runtime/scene)
add_subdirectory(operations)
add_subdirectory(rendering/frame)
add_subdirectory(development)
add_subdirectory(desktop/viewport/api)
add_subdirectory(desktop/config)
add_subdirectory(desktop/models)
add_subdirectory(desktop/session)
add_subdirectory(adapters/pdal)
add_subdirectory(adapters/gdal/runtime)
add_subdirectory(adapters/gdal/raster)
add_subdirectory(adapters/ogr)
add_subdirectory(desktop/dispatch)
add_subdirectory(desktop/operations)
add_subdirectory(desktop/ui)
add_subdirectory(rendering/rhi)
add_subdirectory(desktop/viewport)

qt_add_executable(pcinspector
        "${PROJECT_SOURCE_DIR}/main.cpp"
        desktop/bootstrap/src/ApplicationBootstrap.cpp
        desktop/bootstrap/include/pci/desktop/bootstrap/ApplicationBootstrap.h)

set(pcinspector_runtime_icon
        "${PROJECT_SOURCE_DIR}/assets/icons/pcinspector-256.png")
qt_add_resources(pcinspector application_icon
        PREFIX "/icons"
        BASE "${PROJECT_SOURCE_DIR}/assets/icons"
        FILES "${pcinspector_runtime_icon}")

if(APPLE)
    set(pcinspector_macos_icon
            "${PROJECT_SOURCE_DIR}/assets/icons/platform/macos/PointCloudInspector.icns")
    set_source_files_properties("${pcinspector_macos_icon}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
    target_sources(pcinspector PRIVATE "${pcinspector_macos_icon}")
elseif(WIN32)
    target_sources(pcinspector PRIVATE
            "${PROJECT_SOURCE_DIR}/assets/icons/platform/windows/PointCloudInspector.rc")
endif()

target_link_libraries(pcinspector PRIVATE
        pcinspector::app_config
        pcinspector::app_ui
        pcinspector::color
        pcinspector::development_support
        pcinspector::desktop_operations
        pcinspector::import_gdal
        pcinspector::import_ogr
        pcinspector::import_pdal
        pcinspector::operation_local
        pcinspector::platform
        pcinspector::viewport)
target_link_libraries(pcinspector PRIVATE pcinspector::secure_storage)
target_compile_definitions(pcinspector PRIVATE
        PCINSPECTOR_VERSION="${PROJECT_VERSION}")
set_target_properties(pcinspector PROPERTIES
        OUTPUT_NAME pcinspector
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "${PCINSPECTOR_APPLICATION_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${PCINSPECTOR_APPLICATION_ID}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "PointCloudInspector.icns")
if(APPLE)
    set_target_properties(pcinspector PROPERTIES
            OUTPUT_NAME "${PCINSPECTOR_APPLICATION_NAME}")
endif()
target_include_directories(pcinspector PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/desktop/bootstrap/include")
pci_configure_target(pcinspector)

pci_add_point_shaders(pcinspector point_shaders)
pci_add_cpt_resources(pcinspector embedded_color_maps)

include("${PROJECT_SOURCE_DIR}/cmake/PciPublicHeaders.cmake")
