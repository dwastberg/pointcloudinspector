set(pcinspector_source_include_dir "${PROJECT_SOURCE_DIR}/src")

add_library(pcinspector_foundation STATIC
        foundation/Bounds3d.cpp
        foundation/Bounds3d.h
        foundation/CheckedArithmetic.h
        foundation/Hash.h
        foundation/SpatialReferenceComparator.h
        foundation/StrongId.h
        foundation/Vec3d.h)
add_library(pcinspector::foundation ALIAS pcinspector_foundation)
target_include_directories(pcinspector_foundation PUBLIC
        "${pcinspector_source_include_dir}")
pci_configure_target(pcinspector_foundation)

add_library(pcinspector_platform STATIC
        platform/ProcessMemory.cpp
        platform/ProcessMemory.h
        platform/SystemMemoryInfo.cpp
        platform/SystemMemoryInfo.h)
add_library(pcinspector::platform ALIAS pcinspector_platform)
target_include_directories(pcinspector_platform PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_platform PRIVATE
        pcinspector::foundation)
pci_configure_target(pcinspector_platform)
if(WIN32)
    target_link_libraries(pcinspector_platform PRIVATE Psapi)
endif()

add_library(pcinspector_navigation STATIC
        navigation/NavigationCamera.cpp
        navigation/NavigationCamera.h
        navigation/NavigationInputState.cpp
        navigation/NavigationInputState.h)
add_library(pcinspector::navigation ALIAS pcinspector_navigation)
target_include_directories(pcinspector_navigation PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_navigation PUBLIC
        pcinspector::foundation)
pci_configure_target(pcinspector_navigation)

add_library(pcinspector_renderer_planning STATIC
        renderer/planning/AdaptivePointBudget.cpp
        renderer/planning/AdaptivePointBudget.h
        renderer/planning/FrameCamera.cpp
        renderer/planning/FrameCamera.h
        renderer/planning/FullDetailController.cpp
        renderer/planning/FullDetailController.h
        renderer/planning/FramePlanner.cpp
        renderer/planning/FramePlanner.h
        renderer/planning/FrustumCuller.cpp
        renderer/planning/FrustumCuller.h
        renderer/planning/Measurement.cpp
        renderer/planning/Measurement.h
        renderer/planning/MeasurementController.cpp
        renderer/planning/MeasurementController.h
        renderer/planning/PointCloudSourceBoundsIndex.cpp
        renderer/planning/PointCloudSourceBoundsIndex.h
        renderer/planning/PointFrameCoordinator.cpp
        renderer/planning/PointFrameCoordinator.h
        renderer/planning/RasterLodPlanner.cpp
        renderer/planning/RasterLodPlanner.h
        renderer/planning/RenderActivity.h
        renderer/planning/RenderSelection.cpp
        renderer/planning/RenderSelection.h
        renderer/planning/SceneVisibilityIndex.cpp
        renderer/planning/SceneVisibilityIndex.h)
add_library(pcinspector::renderer_planning ALIAS
        pcinspector_renderer_planning)
target_include_directories(pcinspector_renderer_planning PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_renderer_planning PUBLIC
        pcinspector::foundation)
pci_configure_target(pcinspector_renderer_planning)

add_library(pcinspector_pointcloud STATIC
        pointcloud/CptColorMapParser.cpp
        pointcloud/CptColorMapParser.h
        pointcloud/GpuPoint.h
        pointcloud/GpuPointProperties.h
        pointcloud/PointAttributes.h
        pointcloud/PointClassificationFilter.h
        pointcloud/PointCloudMetadata.h
        pointcloud/PointColorMapCatalog.cpp
        pointcloud/PointColorMapCatalog.h
        pointcloud/PointColorPolicy.cpp
        pointcloud/PointColorPolicy.h
        pointcloud/SourcePoint.h)
add_library(pcinspector::pointcloud ALIAS pcinspector_pointcloud)
target_include_directories(pcinspector_pointcloud PUBLIC
        "${pcinspector_source_include_dir}")
pci_configure_target(pcinspector_pointcloud)
target_link_libraries(pcinspector_pointcloud PUBLIC
        pcinspector::foundation)

# Portable raster domain. It reaches pointcloud for CPT stop tables only, and
# must never depend on scene, renderer, Qt, or GDAL.
add_library(pcinspector_raster STATIC
        raster/RasterLayer.cpp
        raster/RasterLayer.h
        raster/RasterPointSampler.cpp
        raster/RasterPointSampler.h
        raster/RasterTileCache.cpp
        raster/RasterTileCache.h
        raster/RasterTileSource.h)
add_library(pcinspector::raster ALIAS pcinspector_raster)
target_include_directories(pcinspector_raster PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_raster PUBLIC
        pcinspector::foundation
        pcinspector::pointcloud)
pci_configure_target(pcinspector_raster)

add_library(pcinspector_earcut INTERFACE)
add_library(pcinspector::earcut ALIAS pcinspector_earcut)
target_include_directories(pcinspector_earcut SYSTEM INTERFACE
        "${PROJECT_SOURCE_DIR}/third_party/earcut/include")

add_library(pcinspector_vector STATIC
        vector/VectorGeometry.cpp
        vector/VectorGeometry.h
        vector/VectorLayerData.cpp
        vector/VectorLayerData.h
        vector/VectorLayerStyle.cpp
        vector/VectorLayerStyle.h
        vector/VectorImport.h)
add_library(pcinspector::vector ALIAS pcinspector_vector)
target_include_directories(pcinspector_vector PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_vector
        PUBLIC pcinspector::foundation
        PRIVATE pcinspector::earcut)
pci_configure_target(pcinspector_vector)

add_library(pcinspector_tasking STATIC
        tasking/TaskScheduler.cpp
        tasking/TaskScheduler.h)
add_library(pcinspector::tasking ALIAS pcinspector_tasking)
target_include_directories(pcinspector_tasking PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_tasking PUBLIC
        pcinspector::foundation)
pci_configure_target(pcinspector_tasking)

add_library(pcinspector_scene STATIC
        scene/BlockPartitioner.cpp
        scene/BlockPartitioner.h
        scene/DecodedCacheMetrics.h
        scene/DecodedPageCache.cpp
        scene/DecodedPageCache.h
        scene/HierarchyResidencyCoordinator.cpp
        scene/HierarchyResidencyCoordinator.h
        scene/PointBlock.cpp
        scene/PointBlock.h
        scene/PointCloudDataSource.h
        scene/SceneDocument.cpp
        scene/SceneDocument.h
        scene/SceneDocumentSnapshot.cpp
        scene/SceneDocumentSnapshot.h
        scene/PointCloudNode.cpp
        scene/PointCloudNode.h
        scene/RasterColorizeRunStore.cpp
        scene/RasterColorizeRunStore.h
        scene/RasterColorizedPointSource.cpp
        scene/RasterColorizedPointSource.h
        scene/RasterLayerDisplay.cpp
        scene/RasterLayerDisplay.h
        scene/RasterPointColorize.cpp
        scene/RasterPointColorize.h
        scene/RasterPointColorizer.cpp
        scene/RasterPointColorizer.h
        scene/SceneBlock.h
        scene/PointMemoryBudget.cpp
        scene/PointMemoryBudget.h
        scene/PointCloudScene.cpp
        scene/PointCloudScene.h
        scene/PointCloudSceneSnapshot.h)
add_library(pcinspector::scene ALIAS pcinspector_scene)
target_include_directories(pcinspector_scene PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_scene PUBLIC
        pcinspector::foundation
        pcinspector::pointcloud
        pcinspector::raster
        pcinspector::tasking
        pcinspector::vector)
pci_configure_target(pcinspector_scene)
target_link_libraries(pcinspector_renderer_planning PUBLIC
        pcinspector::pointcloud
        pcinspector::scene)

add_library(pcinspector_development_support STATIC
        development/SyntheticPointCloud.cpp
        development/SyntheticPointCloud.h
        development/SyntheticScene.cpp
        development/SyntheticScene.h)
add_library(pcinspector::development_support ALIAS
        pcinspector_development_support)
target_include_directories(pcinspector_development_support PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_development_support PUBLIC
        pcinspector::pointcloud
        pcinspector::scene)
pci_configure_target(pcinspector_development_support)

# Header-only contracts keep implementation dependencies out of public APIs.
add_library(pcinspector_import_api INTERFACE)
add_library(pcinspector::import_api ALIAS pcinspector_import_api)
target_sources(pcinspector_import_api INTERFACE
        import/PointCloudImport.h
        import/PointCloudStatistics.h)
target_include_directories(pcinspector_import_api INTERFACE
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_import_api INTERFACE
        pcinspector::project_options
        pcinspector::scene)

add_library(pcinspector_renderer_api INTERFACE)
add_library(pcinspector::renderer_api ALIAS pcinspector_renderer_api)
target_sources(pcinspector_renderer_api INTERFACE
        renderer/GraphicsApi.h
        renderer/PointColorLabels.h
        renderer/RenderLoadProgress.h
        renderer/RenderMetrics.h
        renderer/RenderViewport.h)
target_include_directories(pcinspector_renderer_api INTERFACE
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_renderer_api INTERFACE
        pcinspector::project_options
        pcinspector::scene
        Qt6::Core)

add_library(pcinspector_app_config STATIC
        app/ApplicationConfig.cpp
        app/ApplicationConfig.h
        app/PerformanceSettings.cpp
        app/PerformanceSettings.h
        app/ApplicationOptions.cpp
        app/ApplicationOptions.h
        app/MemoryBudgetPolicy.cpp
        app/MemoryBudgetPolicy.h)
add_library(pcinspector::app_config ALIAS pcinspector_app_config)
target_include_directories(pcinspector_app_config PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_app_config PUBLIC
        pcinspector::foundation
        pcinspector::platform
        pcinspector::renderer_api)
pci_configure_target(pcinspector_app_config)

add_library(pcinspector_app_model STATIC
        app/LoadingProgressModel.cpp
        app/LoadingProgressModel.h)
add_library(pcinspector::app_model ALIAS pcinspector_app_model)
target_include_directories(pcinspector_app_model PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_app_model PUBLIC
        pcinspector::import_api
        pcinspector::renderer_api)
pci_configure_target(pcinspector_app_model)

add_library(pcinspector_app_session STATIC
        app/SceneSession.cpp
        app/SceneSession.h)
add_library(pcinspector::app_session ALIAS pcinspector_app_session)
target_include_directories(pcinspector_app_session PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_app_session PUBLIC
        pcinspector::app_config
        pcinspector::app_model
        pcinspector::import_async
        pcinspector::scene
        Qt6::Core
        PRIVATE
        pcinspector::platform)
set_target_properties(pcinspector_app_session PROPERTIES AUTOMOC ON)
pci_configure_target(pcinspector_app_session)

add_library(pcinspector_import_pdal STATIC
        import/local/LocalPointIndexBuilder.h
        import/local/LocalPointPageFormat.cpp
        import/local/LocalPointPageFormat.h
        import/local/LocalPointPageSource.cpp
        import/local/LocalPointPageSource.h
        import/pdal/PdalHierarchicalPointSource.cpp
        import/pdal/PdalHierarchicalPointSource.h
        import/pdal/LocalPageBuildInfrastructure.cpp
        import/pdal/LocalPageBuildInfrastructure.h
        import/pdal/LeafPageWriter.cpp
        import/pdal/LeafPageWriter.h
        import/pdal/MortonRunBuilder.cpp
        import/pdal/MortonRunBuilder.h
        import/pdal/MortonRunRecord.h
        import/pdal/ParentHierarchyBuilder.cpp
        import/pdal/ParentHierarchyBuilder.h
        import/pdal/LocalPointIndexBuilder.cpp
        import/pdal/PdalPointMapping.cpp
        import/pdal/PdalPointMapping.h
        import/pdal/PdalPointCloudLoader.cpp
        import/pdal/PdalPointCloudLoader.h
        import/pdal/PdalPointCloudStatistics.cpp
        import/pdal/PdalPointCloudStatistics.h
        import/pdal/PdalSourceInspector.cpp
        import/pdal/PdalSourceInspector.h)
add_library(pcinspector::import_pdal ALIAS pcinspector_import_pdal)
target_include_directories(pcinspector_import_pdal PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_import_pdal
        PUBLIC
        pcinspector::import_api
        PRIVATE
        Qt6::Core
        PDAL::pdalcpp)
pci_configure_target(pcinspector_import_pdal)

# The single owner of the GDAL::GDAL edge. Both the raster and OGR adapters
# reach GDAL through this target so driver registration, error capture, and the
# process-global block cache have exactly one home.
add_library(pcinspector_gdal_runtime STATIC
        import/gdal/GdalRuntime.cpp
        import/gdal/GdalRuntime.h)
add_library(pcinspector::gdal_runtime ALIAS pcinspector_gdal_runtime)
target_include_directories(pcinspector_gdal_runtime PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_gdal_runtime PUBLIC
        GDAL::GDAL)
pci_configure_target(pcinspector_gdal_runtime)

add_library(pcinspector_import_gdal STATIC
        import/RasterImport.h
        import/gdal/GdalRasterDataset.cpp
        import/gdal/GdalRasterDataset.h
        import/gdal/GdalRasterLoader.cpp
        import/gdal/GdalRasterLoader.h
        import/gdal/GdalSpatialReferenceComparator.cpp
        import/gdal/GdalSpatialReferenceComparator.h
        import/gdal/GdalRasterSource.cpp
        import/gdal/GdalRasterSource.h)
add_library(pcinspector::import_gdal ALIAS pcinspector_import_gdal)
target_include_directories(pcinspector_import_gdal PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_import_gdal
        PUBLIC
        pcinspector::raster
        PRIVATE
        pcinspector::gdal_runtime)
pci_configure_target(pcinspector_import_gdal)

add_library(pcinspector_import_ogr STATIC
        import/ogr/OgrVectorLoader.cpp
        import/ogr/OgrVectorLoader.h)
add_library(pcinspector::import_ogr ALIAS pcinspector_import_ogr)
target_include_directories(pcinspector_import_ogr PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_import_ogr
        PUBLIC
        pcinspector::vector
        PRIVATE
        pcinspector::gdal_runtime)
pci_configure_target(pcinspector_import_ogr)

add_library(pcinspector_import_async STATIC
        import/JobResult.h
        import/LoadJobKey.h
        import/LoadJobRow.cpp
        import/LoadJobRow.h
        import/LoadJobMechanics.h
        import/PointCloudImport.h
        import/PointCloudColorizeController.cpp
        import/PointCloudColorizeController.h
        import/PointCloudLoadController.cpp
        import/PointCloudLoadController.h
        import/RasterImport.h
        import/RasterLoadController.cpp
        import/RasterLoadController.h
        import/VectorLoadController.cpp
        import/VectorLoadController.h
        import/ImportServices.h)
add_library(pcinspector::import_async ALIAS pcinspector_import_async)
target_include_directories(pcinspector_import_async PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_import_async
        PUBLIC
        pcinspector::import_api
        pcinspector::raster
        pcinspector::tasking
        Qt6::Core
        PRIVATE
        pcinspector::platform)
set_target_properties(pcinspector_import_async PROPERTIES AUTOMOC ON)
pci_configure_target(pcinspector_import_async)

add_library(pcinspector_app_ui STATIC
        app/ColorizeFromRasterDialog.cpp
        app/ColorizeFromRasterDialog.h
        app/ClassificationFilterDialog.cpp
        app/ClassificationFilterDialog.h
        app/DiagnosticsDock.cpp
        app/DiagnosticsDock.h
        app/EmbeddedColorMaps.cpp
        app/EmbeddedColorMaps.h
        app/LoadingOverlay.cpp
        app/LoadingOverlay.h
        app/LayerInspectorDock.cpp
        app/LayerInspectorDock.h
        app/MainWindow.cpp
        app/MainWindow.h
        app/PointCloudLoadChoiceDialog.cpp
        app/PointCloudLoadChoiceDialog.h
        app/PointCloudLoadMode.h
        app/PointCloudStatisticsDialog.cpp
        app/PointCloudStatisticsDialog.h
        app/PerformanceSettingsStore.cpp
        app/PerformanceSettingsStore.h
        app/QualificationReporter.cpp
        app/QualificationReporter.h
        app/RasterColorizeUiState.cpp
        app/RasterColorizeUiState.h
        app/RenderDiagnosticsFormatter.cpp
        app/RenderDiagnosticsFormatter.h
        app/SceneLayersDock.cpp
        app/SceneLayersDock.h
        app/SceneLayerListModel.cpp
        app/SceneLayerListModel.h
        app/SceneLayerMetaType.h
        app/SettingsDialog.cpp
        app/SettingsDialog.h
        app/StrataTheme.cpp
        app/StrataTheme.h
        app/TaskDock.cpp
        app/TaskDock.h
        app/TaskListModel.cpp
        app/TaskListModel.h
        app/ToolbarIcons.cpp
        app/ToolbarIcons.h
        app/VectorColorButton.cpp
        app/VectorColorButton.h
        app/VectorSublayerDialog.cpp
        app/VectorSublayerDialog.h
        app/ViewportSettingsStore.cpp
        app/ViewportSettingsStore.h
        app/WorkspaceSettings.cpp
        app/WorkspaceSettings.h)
add_library(pcinspector::app_ui ALIAS pcinspector_app_ui)
target_include_directories(pcinspector_app_ui PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_app_ui PUBLIC
        pcinspector::app_config
        pcinspector::app_model
        pcinspector::app_session
        pcinspector::import_async
        pcinspector::pointcloud
        pcinspector::renderer_api
        pcinspector::scene
        pcinspector::vector
        Qt6::Widgets
        PRIVATE
        pcinspector::platform
        Qt6::Concurrent)
set_target_properties(pcinspector_app_ui PROPERTIES AUTOMOC ON)
pci_configure_target(pcinspector_app_ui)

add_library(pcinspector_renderer STATIC
        renderer/PointColorMapAtlas.cpp
        renderer/PointColorMapAtlas.h
        renderer/telemetry/RenderTelemetryAccumulator.cpp
        renderer/telemetry/RenderTelemetryAccumulator.h
        renderer/rhi/BackendPolicy.cpp
        renderer/rhi/BackendPolicy.h
        renderer/rhi/EyeDomeLightingPass.cpp
        renderer/rhi/EyeDomeLightingPass.h
        renderer/rhi/PointCloudRenderer.cpp
        renderer/rhi/PointCloudRenderer.h
        renderer/rhi/PointPicker.cpp
        renderer/rhi/PointPicker.h
        renderer/rhi/RasterLayerRenderer.cpp
        renderer/rhi/RasterLayerRenderer.h
        renderer/rhi/RasterTileStreamer.cpp
        renderer/rhi/RasterTileStreamer.h
        renderer/rhi/RenderViewportWidget.cpp
        renderer/rhi/RenderViewportWidget_p.h
        renderer/rhi/RhiResource.h
        renderer/rhi/SceneSnapshotCache.cpp
        renderer/rhi/SceneSnapshotCache.h
        renderer/rhi/ShaderLoader.cpp
        renderer/rhi/ShaderLoader.h
        renderer/rhi/VectorLayerRenderer.cpp
        renderer/rhi/VectorLayerRenderer.h
        renderer/rhi/UploadScheduler.cpp
        renderer/rhi/UploadScheduler.h)
add_library(pcinspector::renderer ALIAS pcinspector_renderer)
target_include_directories(pcinspector_renderer PUBLIC
        "${pcinspector_source_include_dir}")
target_link_libraries(pcinspector_renderer
        PUBLIC
        pcinspector::renderer_api
        Qt6::Widgets
        PRIVATE
        pcinspector::navigation
        pcinspector::platform
        pcinspector::pointcloud
        pcinspector::raster
        pcinspector::renderer_planning
        pcinspector::scene
        pcinspector::vector
        Qt6::GuiPrivate)
pci_configure_target(pcinspector_renderer)

qt_add_executable(pcinspector
        "${PROJECT_SOURCE_DIR}/main.cpp"
        app/ApplicationBootstrap.cpp
        app/ApplicationBootstrap.h)

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
        pcinspector::development_support
        pcinspector::import_async
        pcinspector::import_gdal
        pcinspector::import_ogr
        pcinspector::import_pdal
        pcinspector::platform
        pcinspector::renderer)
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
pci_configure_target(pcinspector)

pci_add_point_shaders(pcinspector point_shaders)
pci_add_cpt_resources(pcinspector embedded_color_maps)
