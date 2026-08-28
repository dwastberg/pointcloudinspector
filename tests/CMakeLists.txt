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

pci_add_catch_test_target(pcinspector_pointcloud_tests
        WITH_MAIN
        SOURCES
        unit/CptColorMapParserTests.cpp
        unit/PointColorPolicyTests.cpp
        unit/PointCloudContractTests.cpp
        LIBRARIES pcinspector::pointcloud)

pci_add_catch_test_target(pcinspector_scene_tests
        WITH_MAIN
        SOURCES
        unit/PointBlockTests.cpp
        unit/BlockPartitionerTests.cpp
        unit/PointCloudSceneTests.cpp
        unit/PointCloudHierarchyTests.cpp
        unit/RasterColorizeRunStoreTests.cpp
        unit/RasterPointColorizerTests.cpp
        unit/SceneDocumentTests.cpp
        unit/SceneDocumentVectorTests.cpp
        unit/SceneDocumentRasterTests.cpp
        LIBRARIES
        pcinspector::scene
        Qt6::Core)

pci_add_catch_test_target(pcinspector_raster_tests
        WITH_MAIN
        SOURCES
        unit/RasterLayerTests.cpp
        unit/RasterPointSamplerTests.cpp
        unit/RasterTileCacheTests.cpp
        LIBRARIES pcinspector::raster)

pci_add_catch_test_target(pcinspector_foundation_tests
        WITH_MAIN
        SOURCES unit/FoundationTests.cpp
        LIBRARIES pcinspector::foundation)

pci_add_catch_test_target(pcinspector_tasking_tests
        WITH_MAIN
        SOURCES unit/TaskSchedulerTests.cpp
        LIBRARIES pcinspector::tasking)

pci_add_catch_test_target(pcinspector_renderer_planning_tests
        WITH_MAIN
        SOURCES
        renderer/FrustumCullerTests.cpp
        renderer/MeasurementControllerTests.cpp
        renderer/PointFrameCoordinatorTests.cpp
        renderer/PortablePointForestQualificationTests.cpp
        renderer/RenderActivityTests.cpp
        renderer/RenderSelectionTests.cpp
        renderer/RendererPlanningTests.cpp
        renderer/RasterLodPlannerTests.cpp
        renderer/SceneVisibilityIndexTests.cpp
        LIBRARIES pcinspector::renderer_planning)

pci_add_catch_test_target(pcinspector_vector_tests
        WITH_MAIN
        SOURCES
        unit/VectorGeometryTests.cpp
        unit/VectorGeometryLimitsTests.cpp
        unit/VectorLayerStyleTests.cpp
        LIBRARIES pcinspector::vector)

pci_add_catch_test_target(pcinspector_import_pdal_tests
        WITH_MAIN
        SOURCES
        fixtures/PdalFixtureFactory.cpp
        component/PdalImportTests.cpp
        INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        LIBRARIES
        pcinspector::import_pdal
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
        INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        LIBRARIES
        pcinspector::import_gdal
        GDAL::GDAL
        Qt6::Core
        PROPERTIES
        # Catch discovery runs each case in a separate process. Every process
        # rebuilds the shared on-disk corpus, so parallel cases would otherwise
        # delete and rewrite the same TIFF/VRT/GPKG files concurrently.
        RESOURCE_LOCK pcinspector_import_gdal_fixtures)

# The large-source acceptance lane. It links both the GDAL adapter and the
# renderer's streaming code because the bound being asserted spans them: reads
# come from the adapter, admission and eviction from the streamer.
pci_add_catch_test_target(pcinspector_raster_stress_tests
        WITH_MAIN
        INTEGRATION
        SOURCES
        fixtures/GdalRasterFixtureFactory.cpp
        stress/RasterLargeSourceTests.cpp
        stress/RasterPointColorizeScaleTests.cpp
        INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        LIBRARIES
        pcinspector::import_gdal
        pcinspector::renderer
        pcinspector::scene
        pcinspector::secure_storage
        GDAL::GDAL
        PROPERTIES
        RESOURCE_LOCK pcinspector_raster_stress_fixtures
        TIMEOUT 120)

pci_add_catch_test_target(pcinspector_import_ogr_tests
        WITH_MAIN
        SOURCES
        fixtures/OgrFixtureFactory.cpp
        component/OgrVectorImportTests.cpp
        INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        LIBRARIES
        pcinspector::import_ogr
        GDAL::GDAL
        Qt6::Core)

add_executable(pcinspector_ogr_fixture_generator
        fixtures/OgrFixtureFactory.cpp
        generate_ogr_fixtures.cpp)
target_include_directories(pcinspector_ogr_fixture_generator PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pcinspector_ogr_fixture_generator PRIVATE
        GDAL::GDAL)
set_target_properties(pcinspector_ogr_fixture_generator PROPERTIES
        FOLDER Tests)
pci_configure_target(pcinspector_ogr_fixture_generator)

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
add_test(NAME generate_pdal_fixtures
        COMMAND $<TARGET_FILE:pcinspector_fixture_generator>
                "${PCINSPECTOR_TEST_DATA_DIR}")
set_tests_properties(generate_pdal_fixtures PROPERTIES
        FIXTURES_SETUP phase1_point_files
        LABELS "component;pdal")

set(PCINSPECTOR_VECTOR_TEST_DATA_DIR "${PCINSPECTOR_TEST_DATA_DIR}/vector")
add_test(NAME generate_ogr_fixtures
        COMMAND $<TARGET_FILE:pcinspector_ogr_fixture_generator>
                "${PCINSPECTOR_VECTOR_TEST_DATA_DIR}")
set_tests_properties(generate_ogr_fixtures PROPERTIES
        FIXTURES_SETUP phase1_vector_files
        LABELS "component;ogr")

if(TARGET pci_residency_bench)
    add_test(NAME residency_bench_fixture_smoke
            COMMAND $<TARGET_FILE:pci_residency_bench>
                    --cpu-cache-mb 1
                    --stress-nodes 8
                    --passes 1
                    "${PCINSPECTOR_TEST_DATA_DIR}/fixture.copc.laz")
    set_tests_properties(residency_bench_fixture_smoke PROPERTIES
            FIXTURES_REQUIRED phase1_point_files
            PASS_REGULAR_EXPRESSION
                "RESIDENCY_RESULT status=insufficient_hierarchy.*bounded=1"
            LABELS "benchmark;component;hierarchy;stress"
            TIMEOUT 30)

    add_test(NAME residency_bench_local_hierarchy_stress
            COMMAND $<TARGET_FILE:pci_residency_bench>
                    --cpu-cache-mb 1
                    --stress-nodes 160
                    --passes 1
                    --minimum-working-set-multiple 10
                    "${PCINSPECTOR_TEST_DATA_DIR}/residency-stress.copc.laz")
    set_tests_properties(residency_bench_local_hierarchy_stress PROPERTIES
            FIXTURES_REQUIRED phase1_point_files
            PASS_REGULAR_EXPRESSION
                "RESIDENCY_RESULT status=ok.*bounded=1.*cache_evictions=[1-9][0-9]*"
            LABELS "benchmark;component;hierarchy;residency;stress"
            TIMEOUT 60)
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
            FIXTURES_REQUIRED phase1_point_files
            PASS_REGULAR_EXPRESSION
                "MULTIFILE_RESULT status=ok sources=2 previews=2.*bounded=1"
            LABELS "benchmark;component;local-pages;multi-file;stress"
            TIMEOUT 45)
endif()

if(TARGET pci_qualification_diff AND
        EXISTS "${PROJECT_SOURCE_DIR}/qualification/tolerances.json" AND
        EXISTS "${PROJECT_SOURCE_DIR}/qualification/release-h-local-two-file.json" AND
        EXISTS "${PROJECT_SOURCE_DIR}/qualification/release-h-local-two-file-reopen.json" AND
        EXISTS "${PROJECT_SOURCE_DIR}/qualification/release-h-native-metal-final.json")
    set(pcinspector_qualification_directory
        "${PROJECT_SOURCE_DIR}/qualification")
    set(pcinspector_qualification_tolerances
        "${pcinspector_qualification_directory}/tolerances.json")
    foreach(report IN ITEMS
            release-h-local-two-file.json
            release-h-local-two-file-reopen.json
            release-h-native-metal-final.json)
        string(REPLACE ".json" "" report_name "${report}")
        add_test(
            NAME "qualification_diff_${report_name}_self"
            COMMAND $<TARGET_FILE:pci_qualification_diff>
                    "${pcinspector_qualification_directory}/${report}"
                    "${pcinspector_qualification_directory}/${report}"
                    "${pcinspector_qualification_tolerances}")
        set_tests_properties(
            "qualification_diff_${report_name}_self"
            PROPERTIES LABELS "qualification;tools")
    endforeach()

    foreach(check IN ITEMS ignored regression mismatch missing)
        if(check STREQUAL "ignored")
            set(expected_exit 0)
            set(expected_output "PASS first_point_any")
        elseif(check STREQUAL "regression")
            set(expected_exit 5)
            set(expected_output "REGRESSION sampled_frame_p95")
        elseif(check STREQUAL "mismatch")
            set(expected_exit 4)
            set(expected_output "WORKLOAD_MISMATCH gpu_budget_bytes")
        else()
            set(expected_exit 3)
            set(expected_output "SCHEMA_ERROR candidate: required field 'frame_ms_p95'")
        endif()
        add_test(
            NAME "qualification_diff_${check}_self_check"
            COMMAND "${CMAKE_COMMAND}"
                    "-DQUALIFICATION_DIFF=$<TARGET_FILE:pci_qualification_diff>"
                    "-DBASELINE=${pcinspector_qualification_directory}/release-h-native-metal-final.json"
                    "-DTOLERANCES=${pcinspector_qualification_tolerances}"
                    "-DMODE=${check}"
                    "-DEXPECTED_EXIT=${expected_exit}"
                    "-DEXPECTED_OUTPUT=${expected_output}"
                    -P
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunQualificationDiffCheck.cmake")
        set_tests_properties(
            "qualification_diff_${check}_self_check"
            PROPERTIES LABELS "qualification;tools")
    endforeach()
    unset(expected_exit)
    unset(expected_output)
    unset(pcinspector_qualification_directory)
    unset(pcinspector_qualification_tolerances)
endif()

pci_add_catch_test_target(pcinspector_qt_async_tests
        SOURCES
        support/CatchQCoreMain.cpp
        qt/PointCloudLoadControllerTests.cpp
        qt/VectorLoadControllerTests.cpp
        qt/RasterLoadControllerTests.cpp
        qt/RasterElevationControllerTests.cpp
        LIBRARIES
        pcinspector::import_async
        Qt6::Test)

pci_add_catch_test_target(pcinspector_scene_session_tests
        SOURCES
        support/CatchQCoreMain.cpp
        qt/SceneSessionTests.cpp
        LIBRARIES
        pcinspector::app_session
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
        ui/VectorColorButtonTests.cpp
        ui/VectorSublayerDialogTests.cpp
        ui/MainWindowTests.cpp
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
        qt/RenderTelemetryAccumulatorTests.cpp
    qt/VectorLayerRendererTests.cpp
        qt/RasterLayerRendererTests.cpp
        qt/RasterTileStreamerTests.cpp
        qt/UploadSchedulerTests.cpp
        qt/PointPickResultTests.cpp
        LIBRARIES
        pcinspector::renderer
        Qt6::GuiPrivate
        Qt6::Test)

pci_add_catch_test_target(pcinspector_renderer_ui_tests
        INTEGRATION
        SOURCES
        support/CatchWidgetsMain.cpp
        ui/RendererContractTests.cpp
        LIBRARIES
        pcinspector::development_support
        pcinspector::renderer
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
        INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        LIBRARIES
        pcinspector::development_support
        pcinspector::renderer
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
            pcinspector::renderer
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
                FIXTURES_REQUIRED phase1_point_files
                PASS_REGULAR_EXPRESSION "Loaded .*: 8 / 8 points"
                LABELS "gpu;smoke"
                RUN_SERIAL TRUE
                TIMEOUT 20)
    endforeach()
endif()
