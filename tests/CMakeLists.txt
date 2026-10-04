set(CATCH_BUILD_TESTING OFF CACHE BOOL
        "Build Catch2's own tests" FORCE)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL
        "Install Catch2 documentation" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/Catch2"
                 "${CMAKE_CURRENT_BINARY_DIR}/Catch2"
                 EXCLUDE_FROM_ALL)
# Catch2 exposes feature-dependent symbols (including std::string_view
# formatters) from its compiled library. Compile it at the same language level
# as its consumers so headers and library agree on that ABI surface.
target_compile_features(Catch2 PUBLIC cxx_std_23)
# Instrument the compiled test framework consistently with test consumers.
# Mixed libc++ vector instantiations otherwise disagree about ASan container
# annotations (observed in Catch::StringStreams during reporter destruction).
target_link_libraries(Catch2 PRIVATE pcinspector::project_options)
target_link_libraries(Catch2WithMain PRIVATE pcinspector::project_options)
set_target_properties(Catch2 Catch2WithMain PROPERTIES CXX_EXTENSIONS OFF)
list(APPEND CMAKE_MODULE_PATH
        "${PROJECT_SOURCE_DIR}/third_party/Catch2/extras")
include(Catch)
include("${PROJECT_SOURCE_DIR}/cmake/PciTesting.cmake")

pci_add_catch_test_target(pcinspector_app_config_tests
        WITH_MAIN
        SOURCES unit/ApplicationConfigTests.cpp
        LIBRARIES
        pcinspector::app_config
        Qt6::Core)

pci_add_catch_test_target(pcinspector_app_model_tests
        WITH_MAIN
        SOURCES unit/LoadingProgressModelTests.cpp
        LIBRARIES pcinspector::app_model)

pci_add_catch_test_target(pcinspector_development_support_tests
        WITH_MAIN
        SOURCES
        unit/DevelopmentSupportTests.cpp
        unit/SyntheticSceneTests.cpp
        LIBRARIES pcinspector::development_support)

pci_add_catch_test_target(pcinspector_navigation_tests
        WITH_MAIN
        SOURCES
        unit/NavigationCameraTests.cpp
        unit/NavigationCameraSceneTests.cpp
        unit/NavigationInputStateTests.cpp
        unit/QualificationCameraPathTests.cpp
        LIBRARIES pcinspector::navigation)

pci_add_catch_test_target(pcinspector_platform_tests
        WITH_MAIN
        SOURCES unit/PlatformTests.cpp
        LIBRARIES pcinspector::platform)

pci_add_catch_test_target(pcinspector_supported_source_tests
        WITH_MAIN
        SOURCES unit/SupportedSourceTests.cpp
        LIBRARIES pcinspector::app_config)

pci_add_catch_test_target(pcinspector_secure_storage_tests
        WITH_MAIN
        SOURCES unit/SecureStorageTests.cpp
        LIBRARIES pcinspector::secure_storage)

pci_add_catch_test_target(pcinspector_color_tests
        WITH_MAIN
        SOURCES
        unit/ColorRampTests.cpp
        unit/CptColorMapParserTests.cpp
        LIBRARIES pcinspector::color)

pci_add_catch_test_target(pcinspector_pointcloud_tests
        WITH_MAIN
        SOURCES
        unit/PointBlockTests.cpp
        unit/PointBlockPartitionerTests.cpp
        unit/PointColorPolicyTests.cpp
        unit/PointCloudContractTests.cpp
        LIBRARIES pcinspector::pointcloud)

pci_add_catch_test_target(pcinspector_document_tests
        WITH_MAIN
        SOURCES
        unit/SceneDocumentTests.cpp
        unit/SceneDocumentVectorTests.cpp
        unit/SceneDocumentRasterTests.cpp
        LIBRARIES
        pcinspector::document
        Qt6::Core)

pci_add_catch_test_target(pcinspector_scene_runtime_tests
        WITH_MAIN
        SOURCES unit/SceneRuntimeTests.cpp
        LIBRARIES pcinspector::scene_runtime)

pci_add_catch_test_target(pcinspector_operations_tests
        WITH_MAIN
        SOURCES unit/RasterPointColorizerTests.cpp
                unit/OperationRegistryTests.cpp
                unit/StatisticsOperationTests.cpp
                unit/StorageMaintenanceOperationTests.cpp
                unit/PointDatasetIngestionTests.cpp
        LIBRARIES
        pcinspector::operations
        Qt6::Core)

pci_add_catch_test_target(pcinspector_point_runtime_tests
        WITH_MAIN
        SOURCES
        unit/PointDatasetRuntimeTests.cpp
        unit/PointCloudHierarchyTests.cpp
        unit/PointResidencyServiceTests.cpp
        unit/DecodedPageCacheTests.cpp
        LIBRARIES pcinspector::point_runtime)

pci_add_catch_test_target(pcinspector_raster_tests
        WITH_MAIN
        SOURCES
        unit/RasterLayerTests.cpp
        unit/RasterLayerDisplayTests.cpp
        unit/RasterPointSamplerTests.cpp
        LIBRARIES pcinspector::raster)

pci_add_catch_test_target(pcinspector_raster_runtime_tests
        WITH_MAIN
        SOURCES
        raster_runtime/RasterTileCacheTests.cpp
        raster_runtime/RasterTileStreamerTests.cpp
        LIBRARIES pcinspector::raster_runtime)

pci_add_catch_test_target(pcinspector_foundation_tests
        WITH_MAIN
        SOURCES unit/FoundationTests.cpp
        LIBRARIES pcinspector::foundation)

pci_add_catch_test_target(pcinspector_render_telemetry_tests
        WITH_MAIN
        SOURCES unit/RenderTelemetryTests.cpp
        LIBRARIES pcinspector::render_telemetry)

pci_add_catch_test_target(pcinspector_tasking_tests
        WITH_MAIN
        SOURCES unit/TaskSchedulerTests.cpp
        LIBRARIES pcinspector::tasking
        PROPERTIES TIMEOUT 60)

pci_add_catch_test_target(pcinspector_runtime_resources_tests
        WITH_MAIN
        SOURCES
        unit/CompletionExecutorTests.cpp
        unit/RuntimeResourcesTests.cpp
        LIBRARIES pcinspector::runtime_resources)

pci_add_catch_test_target(pcinspector_operation_api_tests
        WITH_MAIN
        SOURCES unit/RasterColorizeRunStoreContractTests.cpp
        LIBRARIES pcinspector::operation_api)

pci_add_catch_test_target(pcinspector_operation_local_tests
        WITH_MAIN
        SOURCES unit/RasterColorizeRunStoreTests.cpp
        LIBRARIES
        pcinspector::operation_local
        Qt6::Core)

pci_add_catch_test_target(pcinspector_storage_maintenance_tests
        INTEGRATION
        WITH_MAIN
        SOURCES component/StorageMaintenanceTests.cpp
        LIBRARIES pcinspector::operation_local
        pcinspector::secure_storage
        pcinspector::platform
        Qt6::Core
        PROPERTIES TIMEOUT 60)

add_executable(pcinspector_storage_lease_probe support/StorageLeaseProbe.cpp)
target_link_libraries(pcinspector_storage_lease_probe PRIVATE pcinspector::secure_storage pcinspector::platform Qt6::Core)
pci_configure_target(pcinspector_storage_lease_probe)
add_dependencies(pcinspector_storage_maintenance_tests pcinspector_storage_lease_probe)
target_compile_definitions(pcinspector_storage_maintenance_tests PRIVATE PCI_STORAGE_LEASE_PROBE="$<TARGET_FILE:pcinspector_storage_lease_probe>")

pci_add_catch_test_target(pcinspector_renderer_planning_tests
        INTEGRATION
        WITH_MAIN
        SOURCES
        renderer/FrustumCullerTests.cpp
        renderer/MeasurementControllerTests.cpp
        renderer/PointFrameCoordinatorTests.cpp
        renderer/PortablePointForestQualificationTests.cpp
        renderer/RenderActivityTests.cpp
        renderer/RenderSelectionTests.cpp
        renderer/RendererPlanningTests.cpp
        renderer/RasterFrameCoordinatorTests.cpp
        renderer/RasterLodPlannerTests.cpp
        renderer/SceneVisibilityIndexTests.cpp
        LIBRARIES pcinspector::renderer_planning pcinspector::point_runtime)

pci_add_catch_test_target(pcinspector_renderer_execution_tests
        WITH_MAIN
        SOURCES renderer/PointFrameExecutorTests.cpp renderer/FrameExecutorTests.cpp
        LIBRARIES pcinspector::frame)

pci_add_catch_test_target(pcinspector_vector_tests
        WITH_MAIN
        SOURCES
        unit/VectorGeometryTests.cpp
        unit/VectorGeometryLimitsTests.cpp
        unit/VectorLayerStyleTests.cpp
        LIBRARIES pcinspector::vector)

pci_add_catch_test_target(pcinspector_import_pdal_tests
        INTEGRATION
        WITH_MAIN
        SOURCES
        fixtures/PdalFixtureFactory.cpp
        component/PdalImportTests.cpp
        LIBRARIES
        pcinspector::import_pdal
        pcinspector::operations
        pcinspector::operation_local
        Qt6::Core
        PDAL::pdalcpp)

pci_add_catch_test_target(pcinspector_gdal_runtime_tests
        WITH_MAIN
        SOURCES component/GdalRuntimeTests.cpp
        LIBRARIES pcinspector::gdal_runtime)

pci_add_catch_test_target(pcinspector_import_gdal_tests
        WITH_MAIN
        SOURCES
        fixtures/GdalRasterFixtureFactory.cpp
        component/GdalRasterImportTests.cpp
        LIBRARIES
        pcinspector::import_gdal
        GDAL::GDAL
        Qt6::Core)

# The large-source acceptance lane. It links both the GDAL adapter and the
# raster runtime's streaming code because the bound spans them: reads
# come from the adapter, admission and eviction from the streamer.
pci_add_catch_test_target(pcinspector_raster_stress_tests
        ISOLATED LONG_STRESS
        WITH_MAIN
        INTEGRATION
        SOURCES
        fixtures/GdalRasterFixtureFactory.cpp
        stress/RasterLargeSourceTests.cpp
        stress/RasterPointColorizeScaleTests.cpp
        LIBRARIES
        pcinspector::platform
        pcinspector::import_gdal
        pcinspector::operation_local
        pcinspector::operations
        pcinspector::raster_runtime
        pcinspector::renderer
        pcinspector::document
        pcinspector::secure_storage
        GDAL::GDAL
        PROPERTIES
        RESOURCE_LOCK pcinspector_raster_stress_fixtures
        TIMEOUT 120)

pci_add_catch_test_target(pcinspector_import_ogr_tests
        INTEGRATION
        WITH_MAIN
        SOURCES
        fixtures/OgrFixtureFactory.cpp
        component/OgrVectorImportTests.cpp
        LIBRARIES
        pcinspector::import_ogr
        pcinspector::gdal_runtime
        GDAL::GDAL
        Qt6::Core)

if(TARGET pci_residency_bench OR TARGET pci_multifile_bench OR PCINSPECTOR_ENABLE_GPU_TESTS)
    add_executable(pcinspector_fixture_generator
            fixtures/PdalFixtureFactory.cpp
            generate_pdal_fixtures.cpp)
    target_include_directories(pcinspector_fixture_generator PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(pcinspector_fixture_generator PRIVATE
            PDAL::pdalcpp)
    set_target_properties(pcinspector_fixture_generator PROPERTIES
            FOLDER Tests)
    pci_configure_target(pcinspector_fixture_generator)

    set(PCINSPECTOR_TEST_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/data")
    set(pcinspector_fixture_arguments "${PCINSPECTOR_TEST_DATA_DIR}")
    if(PCINSPECTOR_ENABLE_LONG_STRESS_TESTS)
        list(APPEND pcinspector_fixture_arguments --long-stress)
    endif()
    add_test(NAME generate_pdal_fixtures
            COMMAND $<TARGET_FILE:pcinspector_fixture_generator>
                    ${pcinspector_fixture_arguments})
    unset(pcinspector_fixture_arguments)
    set_tests_properties(generate_pdal_fixtures PROPERTIES
            FIXTURES_SETUP pdal_point_files
            LABELS "component;pdal")
endif()

if(TARGET pci_residency_bench)
    add_test(NAME residency_bench_fixture_smoke
            COMMAND $<TARGET_FILE:pci_residency_bench>
                    --cpu-cache-mb 1
                    --stress-nodes 8
                    --passes 1
                    "${PCINSPECTOR_TEST_DATA_DIR}/fixture.copc.laz")
    set_tests_properties(residency_bench_fixture_smoke PROPERTIES
            FIXTURES_REQUIRED pdal_point_files
            PASS_REGULAR_EXPRESSION
                "RESIDENCY_RESULT status=insufficient_hierarchy.*bounded=1"
            LABELS "benchmark;component;hierarchy;stress"
            TIMEOUT 30)

    if(PCINSPECTOR_ENABLE_LONG_STRESS_TESTS)
        add_test(NAME residency_bench_local_hierarchy_stress
                COMMAND $<TARGET_FILE:pci_residency_bench>
                        --cpu-cache-mb 1
                        --stress-nodes 160
                        --passes 1
                        --minimum-working-set-multiple 10
                        "${PCINSPECTOR_TEST_DATA_DIR}/residency-stress.copc.laz")
        set_tests_properties(residency_bench_local_hierarchy_stress PROPERTIES
                FIXTURES_REQUIRED pdal_point_files
                PASS_REGULAR_EXPRESSION
                    "RESIDENCY_RESULT status=ok.*bounded=1.*cache_evictions=[1-9][0-9]*"
                LABELS "benchmark;component;hierarchy;residency;stress;long-stress"
                RUN_SERIAL TRUE
                TIMEOUT 60)
    endif()
endif()

if(TARGET pci_multifile_bench)
    add_test(NAME multifile_bench_local_las_laz_smoke
            COMMAND $<TARGET_FILE:pci_multifile_bench>
                    --cpu-cache-mb 32
                    --local-page-threshold 1
                    --revisit-passes 2
                    --timeout-seconds 30
                    --cache-dir
                    "${PCINSPECTOR_TEST_DATA_DIR}/multifile-page-cache"
                    "${PCINSPECTOR_TEST_DATA_DIR}/fixture.las"
                    "${PCINSPECTOR_TEST_DATA_DIR}/fixture.laz")
    set_tests_properties(multifile_bench_local_las_laz_smoke PROPERTIES
            FIXTURES_REQUIRED pdal_point_files
            PASS_REGULAR_EXPRESSION
                "MULTIFILE_RESULT status=ok sources=2 previews=2.*bounded=1"
            LABELS "benchmark;component;local-pages;multi-file;stress"
            TIMEOUT 45)
endif()

pci_add_catch_test_target(pcinspector_qualification_tests
        WITH_MAIN
        SOURCES
        unit/QualificationDiffTests.cpp
        "${PROJECT_SOURCE_DIR}/tools/qualification/QualificationDiff.cpp"
        INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/tools"
        LIBRARIES pcinspector::platform Qt6::Core)
target_compile_definitions(pcinspector_qualification_tests PRIVATE
        PCI_QUALIFICATION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/qualification")

if(TARGET pci_qualification_diff)
    add_test(NAME qualification_diff_cli_smoke
            COMMAND $<TARGET_FILE:pci_qualification_diff>
                    "${CMAKE_CURRENT_SOURCE_DIR}/data/qualification/native.json"
                    "${CMAKE_CURRENT_SOURCE_DIR}/data/qualification/native.json"
                    "${CMAKE_CURRENT_SOURCE_DIR}/data/qualification/tolerances.json")
    set_tests_properties(qualification_diff_cli_smoke PROPERTIES
            LABELS "qualification;tools"
            PASS_REGULAR_EXPRESSION "PASS sampled_frame_p95")
endif()

pci_add_catch_test_target(pcinspector_qt_async_tests
        INTEGRATION
        SOURCES
        support/CatchQCoreMain.cpp
        qt/PointCloudLoadControllerTests.cpp
        qt/QueuedControllerCallbackTests.cpp
        qt/VectorLoadControllerTests.cpp
        qt/RasterLoadControllerTests.cpp
        qt/RasterElevationControllerTests.cpp
        LIBRARIES
        pcinspector::desktop_operations
        pcinspector::secure_storage
        Qt6::Test)

pci_add_catch_test_target(pcinspector_scene_session_tests
        SOURCES
        support/CatchQCoreMain.cpp
        qt/SceneSessionTests.cpp
        LIBRARIES
        pcinspector::desktop_session
        Qt6::Test)

pci_add_catch_test_target(pcinspector_qt_platform_tests
        SOURCES
        support/CatchQCoreMain.cpp
        qt/QtPathTests.cpp
        LIBRARIES
        pcinspector::platform
        Qt6::Test)

pci_add_catch_test_target(pcinspector_embedded_colormap_tests
        INTEGRATION
        SOURCES
        support/CatchQCoreMain.cpp
        qt/EmbeddedColorMapTests.cpp
        LIBRARIES
        pcinspector::app_ui
        pcinspector::renderer
        Qt6::Test)
pci_add_cpt_resources(
        pcinspector_embedded_colormap_tests test_embedded_color_maps
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/colormaps")

pci_add_catch_test_target(pcinspector_ui_tests
        SOURCES
        support/CatchWidgetsMain.cpp
        ui/ApplicationListModelTests.cpp
        ui/ApplicationHelpersTests.cpp
        ui/ClassificationFilterDialogTests.cpp
        ui/LoadingOverlayTests.cpp
        ui/SettingsDialogTests.cpp
        ui/StorageSettingsTests.cpp
        ui/VectorColorButtonTests.cpp
        ui/VectorSublayerDialogTests.cpp
        ui/MainWindowTests.cpp
        ui/QualificationCriteriaTests.cpp
        ui/LayerDockCharacterizationTests.cpp
        LIBRARIES
        pcinspector::app_ui
        Qt6::Test
        PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

pci_add_catch_test_target(pcinspector_qt_renderer_tests
        SOURCES
        support/CatchQCoreMain.cpp
        qt/BackendPolicyTests.cpp
    qt/PointCloudRendererTests.cpp
        qt/VectorLayerRendererTests.cpp
        qt/RasterLayerRendererTests.cpp
        qt/UploadSchedulerTests.cpp
        qt/PointPickResultTests.cpp
        LIBRARIES
        pcinspector::viewport
        Qt6::GuiPrivate
        Qt6::Test)

pci_add_catch_test_target(pcinspector_render_telemetry_projection_tests
        INTEGRATION
        WITH_MAIN
        SOURCES qt/RenderTelemetryAccumulatorTests.cpp
        LIBRARIES
        pcinspector::render_telemetry
        pcinspector::viewport)

pci_add_catch_test_target(pcinspector_renderer_ui_tests
        INTEGRATION
        SOURCES
        support/CatchWidgetsMain.cpp
        ui/RendererContractTests.cpp
        LIBRARIES
        pcinspector::development_support
        pcinspector::viewport
        Qt6::Test
        PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

pci_add_catch_test_target(pcinspector_renderer_internal_ui_tests
        INTEGRATION
        SOURCES
        support/CatchWidgetsMain.cpp
        renderer_internal/SceneSnapshotCacheTests.cpp
        renderer_internal/RenderViewportWidgetTests.cpp
        renderer_internal/RhiResourceTests.cpp
        LIBRARIES
        pcinspector::development_support
        pcinspector::navigation
        pcinspector::raster_runtime
        pcinspector::render_telemetry
        pcinspector::viewport
        Qt6::GuiPrivate
        Qt6::Test
        PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Runs against the built application rather than a test target, so it measures
# the driver set that actually shipped. Deliberately outside the GPU-test guard
# and free of any display requirement: the packaging lanes, which build with
# diagnostics and GPU tests off, are the ones whose GDAL differs from a
# developer machine's.
if(TARGET pcinspector)
    add_test(NAME gdal_capability_smoke
            COMMAND $<TARGET_FILE:pcinspector> --gdal-capabilities)
    set_tests_properties(gdal_capability_smoke PROPERTIES
            LABELS "component;gdal;packaging"
            PASS_REGULAR_EXPRESSION "driver_gti: yes"
            TIMEOUT 60)
endif()

if(PCINSPECTOR_ENABLE_GPU_TESTS)
    pci_add_catch_test_target(pcinspector_gpu_tests
            ISOLATED
            INTEGRATION
            SOURCES
            support/CatchGpuMain.cpp
            fixtures/GdalRasterFixtureFactory.cpp
            gpu/PointPickerGpuTests.cpp
            gpu/RasterRenderGpuTests.cpp
            gpu/VectorRenderGpuTests.cpp
            LIBRARIES
            pcinspector::app_config
            pcinspector::import_gdal
            pcinspector::navigation
            pcinspector::raster_runtime
            pcinspector::render_telemetry
            pcinspector::viewport
            pcinspector::secure_storage
            GDAL::GDAL
            Qt6::GuiPrivate
            Qt6::Test
            PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 30)
    target_compile_definitions(pcinspector_gpu_tests PRIVATE
            "PCINSPECTOR_GPU_TEST_GRAPHICS_API=\"${PCINSPECTOR_GPU_TEST_GRAPHICS_API}\""
            "PCINSPECTOR_GPU_TEST_VALIDATION=$<BOOL:${PCINSPECTOR_GPU_TEST_VALIDATION}>")
    target_include_directories(pcinspector_gpu_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
    pci_add_point_shaders(
            pcinspector_gpu_tests test_point_shaders)

    add_test(NAME renderer_smoke
            COMMAND $<TARGET_FILE:pcinspector>
                    --smoke-test
                    --graphics-api ${PCINSPECTOR_GPU_TEST_GRAPHICS_API}
                    $<$<BOOL:${PCINSPECTOR_GPU_TEST_VALIDATION}>:--gpu-validation>
                    --points 1000)
    set_tests_properties(renderer_smoke PROPERTIES
            LABELS "gpu;smoke"
            RUN_SERIAL TRUE
            TIMEOUT 20)

    foreach(point_file IN ITEMS
            fixture.las
            fixture.laz
            fixture.copc.laz)
        string(REPLACE "." "_" smoke_name "${point_file}")
        add_test(NAME "renderer_smoke_${smoke_name}"
                COMMAND $<TARGET_FILE:pcinspector>
                        --smoke-test
                        --graphics-api ${PCINSPECTOR_GPU_TEST_GRAPHICS_API}
                        $<$<BOOL:${PCINSPECTOR_GPU_TEST_VALIDATION}>:--gpu-validation>
                        --max-points 8
                        "${PCINSPECTOR_TEST_DATA_DIR}/${point_file}")
        set_tests_properties("renderer_smoke_${smoke_name}" PROPERTIES
                FIXTURES_REQUIRED pdal_point_files
                PASS_REGULAR_EXPRESSION "Loaded .*: 8 / 8 points"
                LABELS "gpu;smoke"
                RUN_SERIAL TRUE
                TIMEOUT 20)
    endforeach()
endif()
