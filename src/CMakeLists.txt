pci_add_module(foundation pcinspector_foundation)
pci_add_module(tasking pcinspector_tasking)
pci_add_module(runtime_resources pcinspector_runtime_resources)
pci_add_module(color pcinspector_color)
pci_add_module(pointcloud pcinspector_pointcloud)
pci_add_module(raster pcinspector_raster)
pci_add_module(raster_runtime pcinspector_raster_runtime)
pci_add_module(vector pcinspector_vector)
pci_add_module(document pcinspector_document)
pci_add_module(operations/api pcinspector_operation_api)
pci_add_module(adapters/platform pcinspector_platform)
pci_add_module(adapters/storage pcinspector_secure_storage)
pci_add_module(operations/adapters/local pcinspector_operation_local)
pci_add_module(navigation pcinspector_navigation)
pci_add_module(rendering/telemetry pcinspector_render_telemetry)
pci_add_module(rendering/planning pcinspector_renderer_planning)
pci_add_module(runtime/point pcinspector_point_runtime)
pci_add_module(runtime/scene pcinspector_scene_runtime)
pci_add_module(operations pcinspector_operations)
pci_add_module(rendering/frame pcinspector_frame)
pci_add_module(development pcinspector_development_support)
pci_add_module(desktop/viewport/api pcinspector_viewport_api)
pci_add_module(desktop/config pcinspector_app_config)
pci_add_module(desktop/models pcinspector_app_model)
pci_add_module(desktop/session pcinspector_desktop_session)
pci_add_module(adapters/pdal pcinspector_import_pdal)
pci_add_module(adapters/gdal/runtime pcinspector_gdal_runtime)
pci_add_module(adapters/gdal/raster pcinspector_import_gdal)
pci_add_module(adapters/ogr pcinspector_import_ogr)
pci_add_module(desktop/dispatch pcinspector_desktop_dispatch)
pci_add_module(desktop/operations pcinspector_desktop_operations)
pci_add_module(desktop/ui pcinspector_app_ui)
pci_add_module(rendering/rhi pcinspector_renderer)
pci_add_module(desktop/viewport pcinspector_viewport)

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
        pcinspector::viewport
        pcinspector::secure_storage)
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
