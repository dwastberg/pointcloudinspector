if(NOT PROJECT_SOURCE_DIR)
    message(FATAL_ERROR "PROJECT_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE project_sources
     LIST_DIRECTORIES FALSE
     "${PROJECT_SOURCE_DIR}/src/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/*.cc"
     "${PROJECT_SOURCE_DIR}/src/*.cxx"
     "${PROJECT_SOURCE_DIR}/src/*.h"
     "${PROJECT_SOURCE_DIR}/tests/*.cpp"
     "${PROJECT_SOURCE_DIR}/tests/*.cc"
     "${PROJECT_SOURCE_DIR}/tests/*.cxx"
     "${PROJECT_SOURCE_DIR}/tests/*.h"
     "${PROJECT_SOURCE_DIR}/tools/*.cpp"
     "${PROJECT_SOURCE_DIR}/tools/*.h")
list(APPEND project_sources "${PROJECT_SOURCE_DIR}/main.cpp")

set(violations "")

foreach(module_cmake IN ITEMS
        foundation/CMakeLists.txt
        color/CMakeLists.txt
        pointcloud/CMakeLists.txt
        raster/CMakeLists.txt
        raster_runtime/CMakeLists.txt
        vector/CMakeLists.txt
        navigation/CMakeLists.txt
        tasking/CMakeLists.txt
        runtime_resources/CMakeLists.txt
        runtime/point/CMakeLists.txt
        runtime/scene/CMakeLists.txt
        document/CMakeLists.txt
        operations/CMakeLists.txt
        desktop/config/CMakeLists.txt
        desktop/models/CMakeLists.txt
        desktop/operations/CMakeLists.txt
        desktop/session/CMakeLists.txt
        desktop/ui/CMakeLists.txt
        desktop/viewport/api/CMakeLists.txt
        operations/api/CMakeLists.txt
        operations/adapters/local/CMakeLists.txt
        rendering/telemetry/CMakeLists.txt
        rendering/planning/CMakeLists.txt
        rendering/rhi/CMakeLists.txt
        rendering/frame/CMakeLists.txt
        adapters/platform/CMakeLists.txt
        adapters/storage/CMakeLists.txt
        adapters/pdal/CMakeLists.txt
        adapters/gdal/runtime/CMakeLists.txt
        adapters/gdal/raster/CMakeLists.txt
        adapters/ogr/CMakeLists.txt
        development/CMakeLists.txt
        desktop/dispatch/CMakeLists.txt
        desktop/viewport/CMakeLists.txt)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/src/${module_cmake}")
        list(APPEND violations
             "src/${module_cmake}: module does not own its CMake target")
    endif()
endforeach()

foreach(point_runtime_file IN ITEMS
        DecodedCacheMetrics.h
        DecodedPageCache.cpp
        DecodedPageCache.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${point_runtime_file}")
        list(APPEND violations
             "src/scene/${point_runtime_file}: point residency code remains owned by the scene facade")
    endif()
endforeach()

foreach(legacy_point_scene_file IN ITEMS
        PointCloudScene.cpp
        PointCloudScene.h
        PointCloudSceneSnapshot.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${legacy_point_scene_file}")
        list(APPEND violations
             "src/scene/${legacy_point_scene_file}: point dataset runtime still uses the document-era scene name")
    endif()
endforeach()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" legacy_point_scene_names REGEX "PointCloudScene")
    if(legacy_point_scene_names)
        list(APPEND violations
             "${relative}: uses the legacy PointCloudScene runtime name")
    endif()
endforeach()

foreach(document_file IN ITEMS
        SceneDocument.cpp
        SceneDocument.h
        SceneDocumentSnapshot.cpp
        SceneDocumentSnapshot.h
        SceneLayer.cpp
        SceneLayer.h
        SceneLayerBounds.cpp
        SceneLayerBounds.h
        SceneLayerTypes.h
        SceneSnapshotLayer.h
        SceneSnapshotLayerView.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${document_file}")
        list(APPEND violations
             "src/scene/${document_file}: metadata-only document code remains owned by the runtime facade")
    endif()
endforeach()

foreach(legacy_support_header IN ITEMS
        navigation/NavigationCamera.h
        navigation/NavigationInputState.h
        navigation/QualificationCameraPath.h
        tasking/TaskScheduler.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/${legacy_support_header}")
        list(APPEND violations
             "src/${legacy_support_header}: public portable header is outside its module include tree")
    endif()
endforeach()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" legacy_support_includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](navigation|tasking)/")
    if(legacy_support_includes)
        list(APPEND violations
             "${relative}: uses a legacy navigation/tasking include path")
    endif()
endforeach()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/CMakeLists.txt"
     root_owned_portable_targets
     REGEX "^add_library.pcinspector_(foundation|color|pointcloud|raster|vector|navigation|tasking|runtime_resources|operation_api|operation_local|render_telemetry)[ \t]")
if(root_owned_portable_targets)
    list(APPEND violations
         "src/CMakeLists.txt: root still owns an extracted portable module target")
endif()

# Generic asynchronous result values are foundation contracts. Keeping this
# declaration under import makes unrelated operation APIs depend on an adapter
# responsibility and recreates an umbrella edge as new job types are added.
if(EXISTS "${PROJECT_SOURCE_DIR}/src/import/JobResult.h")
    list(APPEND violations
         "src/import/JobResult.h: generic job result is owned by import instead of foundation")
endif()

if(NOT EXISTS
        "${PROJECT_SOURCE_DIR}/src/foundation/include/pci/foundation/Generation.h")
    list(APPEND violations
         "src/foundation/include/pci/foundation/Generation.h: distinct session/document/binding generations are missing")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneDocument.h"
     scene_document_identity_declarations
     REGEX "^using (SceneLayerId|PointCloudLayerId)[ \t]*=")
if(scene_document_identity_declarations)
    list(APPEND violations
         "src/document/include/pci/document/SceneDocument.h: shared layer identity is coupled to the document implementation")
endif()

file(STRINGS
     "${PROJECT_SOURCE_DIR}/src/runtime/point/include/pci/runtime/point/DecodedPageCache.h"
     decoded_cache_identity_declarations
     REGEX "^using PointCloudSourceId[ \t]*=")
if(decoded_cache_identity_declarations)
    list(APPEND violations
         "src/runtime/point/include/pci/runtime/point/DecodedPageCache.h: point source identity is coupled to the decoded cache")
endif()

file(STRINGS
     "${PROJECT_SOURCE_DIR}/src/raster/include/pci/raster/RasterLayer.h"
     raster_layer_identity_declarations
     REGEX "^using RasterSourceId[ \t]*=")
if(raster_layer_identity_declarations)
    list(APPEND violations
         "src/raster/RasterLayer.h: raster source identity is coupled to the full layer contract")
endif()

foreach(point_data_header IN ITEMS
        BlockPartitioner.h
        PointBlock.h
        SceneBlock.h
        PointCloudNode.h
        PointCloudDataSource.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${point_data_header}")
        list(APPEND violations
             "src/scene/${point_data_header}: immutable point data contract is owned by scene runtime")
    endif()
endforeach()

if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/BlockPartitioner.cpp")
    list(APPEND violations
         "src/scene/BlockPartitioner.cpp: pure point block construction remains owned by the scene facade")
endif()

file(STRINGS
     "${PROJECT_SOURCE_DIR}/src/operations/api/include/pci/operations/RasterPointColorize.h"
     colorize_contract_runtime_reachability
     REGEX "PointDatasetRuntime")
if(colorize_contract_runtime_reachability)
    list(APPEND violations
         "src/operations/api/include/pci/operations/RasterPointColorize.h: raster-colorization operation values retain a mutable point runtime")
endif()

foreach(colorize_contract_file IN ITEMS RasterPointColorize.h RasterPointColorize.cpp)
    if(EXISTS
            "${PROJECT_SOURCE_DIR}/src/scene/${colorize_contract_file}")
        list(APPEND violations
             "src/scene/${colorize_contract_file}: raster-colorization operation contract remains owned by scene runtime")
    endif()
endforeach()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/operations/api/include/pci/operations/RasterPointColorize.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/operations/api/src/RasterPointColorize.cpp")
    list(APPEND violations
         "operations/api: canonical raster point-colorization contract is missing")
endif()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/pointcloud/include/pci/pointcloud/RasterPointColorData.h")
    list(APPEND violations
         "pointcloud: immutable raster-derived point-color data contract is missing")
else()
    file(STRINGS
         "${PROJECT_SOURCE_DIR}/src/operations/api/include/pci/operations/RasterPointColorize.h"
         operation_owned_point_color_data
         REGEX "^struct Raster(ColorizeHierarchicalTarget|ColorizeFlatTarget|ColorizeTargetSnapshot|ColorizeRange)")
    if(operation_owned_point_color_data)
        list(APPEND violations
             "operations/api/RasterPointColorize.h: immutable point-color target or range data remains owned by operation orchestration")
    endif()
endif()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/runtime/point/include/pci/runtime/point/PointColorInstallation.h")
    list(APPEND violations
         "point_runtime: runtime-owned point-color installation contract is missing")
endif()

file(STRINGS
     "${PROJECT_SOURCE_DIR}/src/operations/api/include/pci/operations/RasterPointColorize.h"
     operation_owned_color_installation
     REGEX "(RasterPointColorApplyOutcome|RasterPointColorizeAvailability|colorizedSource)")
if(operation_owned_color_installation)
    list(APPEND violations
         "operations/api/RasterPointColorize.h: operation result still owns point-runtime installation state")
endif()

file(STRINGS
     "${PROJECT_SOURCE_DIR}/src/runtime/point/include/pci/runtime/point/PointDatasetRuntime.h"
     point_runtime_operation_dependency
     REGEX "pci/operations/")
if(point_runtime_operation_dependency)
    list(APPEND violations
         "runtime/point/PointDatasetRuntime.h: point runtime still depends on operation orchestration")
endif()

foreach(point_runtime_source IN ITEMS
        PointDatasetRuntime.cpp
        PointDatasetRuntime.h
        RasterColorizedPointSource.cpp
        RasterColorizedPointSource.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${point_runtime_source}")
        list(APPEND violations
             "src/scene/${point_runtime_source}: mutable point runtime remains owned by the scene facade")
    endif()
    if(NOT EXISTS
            "${PROJECT_SOURCE_DIR}/src/runtime/point/${point_runtime_source}" AND
       NOT EXISTS
            "${PROJECT_SOURCE_DIR}/src/runtime/point/include/pci/runtime/point/${point_runtime_source}" AND
       NOT EXISTS
            "${PROJECT_SOURCE_DIR}/src/runtime/point/src/${point_runtime_source}")
        list(APPEND violations
             "point_runtime/${point_runtime_source}: canonical mutable point-runtime source is missing")
    endif()
endforeach()

foreach(point_runtime_implementation IN ITEMS
        PointDatasetRuntime.cpp
        RasterColorizedPointSource.cpp)
    set(point_runtime_implementation_path
        "${PROJECT_SOURCE_DIR}/src/runtime/point/src/${point_runtime_implementation}")
    if(EXISTS "${point_runtime_implementation_path}")
        file(STRINGS "${point_runtime_implementation_path}"
             point_runtime_upward_dependency
             REGEX "#[ \t]*include[ \t]*[<\"](scene/|pci/operations/)")
        if(point_runtime_upward_dependency)
            list(APPEND violations
                 "runtime/point/src/${point_runtime_implementation}: point runtime depends on scene or operation orchestration")
        endif()
    endif()
endforeach()

foreach(point_runtime_header IN ITEMS
        PointDatasetRuntime.h
        RasterColorizedPointSource.h)
    set(point_runtime_header_path
        "${PROJECT_SOURCE_DIR}/src/runtime/point/include/pci/runtime/point/${point_runtime_header}")
    if(EXISTS "${point_runtime_header_path}")
        file(STRINGS "${point_runtime_header_path}"
             point_runtime_public_upward_dependency
             REGEX "#[ \t]*include[ \t]*[<\"](scene/|pci/operations/)")
        if(point_runtime_public_upward_dependency)
            list(APPEND violations
                 "runtime/point/include/pci/runtime/point/${point_runtime_header}: public point runtime depends on scene or operation orchestration")
        endif()
    endif()
endforeach()

foreach(colorize_implementation_file IN ITEMS
        RasterPointColorizer.cpp
        RasterPointColorizer.h)
    if(EXISTS
            "${PROJECT_SOURCE_DIR}/src/scene/${colorize_implementation_file}")
        list(APPEND violations
             "src/scene/${colorize_implementation_file}: raster colorization implementation remains owned by the scene facade")
    endif()
endforeach()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/operations/CMakeLists.txt" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/operations/colorize/include/pci/operations/RasterPointColorizer.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/operations/colorize/src/RasterPointColorizer.cpp")
    list(APPEND violations
         "operations/colorize: canonical raster colorization implementation is missing")
endif()

set(colorize_implementation_path
    "${PROJECT_SOURCE_DIR}/src/operations/colorize/src/RasterPointColorizer.cpp")
if(EXISTS "${colorize_implementation_path}")
    file(STRINGS "${colorize_implementation_path}"
         colorize_scene_dependency
         REGEX "#[ \t]*include[ \t]*[<\"]scene/")
    if(colorize_scene_dependency)
        list(APPEND violations
             "operations/colorize: colorization implementation depends on the scene facade")
    endif()
endif()

foreach(scene_runtime_file IN ITEMS SceneRuntime.cpp SceneRuntime.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${scene_runtime_file}")
        list(APPEND violations
             "src/scene/${scene_runtime_file}: source-binding runtime remains owned by the scene facade")
    endif()
endforeach()

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/src/runtime/scene/CMakeLists.txt" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/runtime/scene/include/pci/runtime/scene/SceneRuntime.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/runtime/scene/src/SceneRuntime.cpp")
    list(APPEND violations
         "runtime/scene: canonical source-binding runtime module is missing")
endif()

foreach(scene_runtime_relative_path IN ITEMS
        include/pci/runtime/scene/SceneRuntime.h
        src/SceneRuntime.cpp)
    set(scene_runtime_path
        "${PROJECT_SOURCE_DIR}/src/runtime/scene/${scene_runtime_relative_path}")
    if(EXISTS "${scene_runtime_path}")
        file(STRINGS "${scene_runtime_path}"
             scene_runtime_upward_dependency
             REGEX "#[ \t]*include[ \t]*[<\"](scene/|pci/document/|pci/operations/)")
        if(scene_runtime_upward_dependency)
            list(APPEND violations
                 "runtime/scene/${scene_runtime_relative_path}: source-binding runtime depends on document, scene facade, or operation orchestration")
        endif()
    endif()
endforeach()

foreach(raster_display_file IN ITEMS
        RasterLayerDisplay.cpp
        RasterLayerDisplay.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${raster_display_file}")
        list(APPEND violations
             "src/scene/${raster_display_file}: raster display policy remains owned by the scene facade")
    endif()
endforeach()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/raster/include/pci/raster/RasterLayerDisplay.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/raster/src/RasterLayerDisplay.cpp")
    list(APPEND violations
         "raster: canonical raster display policy is missing")
endif()

foreach(raster_display_relative_path IN ITEMS
        include/pci/raster/RasterLayerDisplay.h
        src/RasterLayerDisplay.cpp)
    set(raster_display_path
        "${PROJECT_SOURCE_DIR}/src/raster/${raster_display_relative_path}")
    if(EXISTS "${raster_display_path}")
        file(STRINGS "${raster_display_path}"
             raster_display_upward_dependency
             REGEX "#[ \t]*include[ \t]*[<\"](scene/|pci/document/|pci/runtime/|pci/operations/)")
        if(raster_display_upward_dependency)
            list(APPEND violations
                 "raster/${raster_display_relative_path}: raster display policy has an upward dependency")
        endif()
    endif()
endforeach()

foreach(raster_cache_legacy_path IN ITEMS
        include/pci/raster/RasterTileCache.h
        src/RasterTileCache.cpp)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/raster/${raster_cache_legacy_path}")
        list(APPEND violations
             "raster/${raster_cache_legacy_path}: decoded tile cache remains owned by the portable raster-data module")
    endif()
endforeach()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/raster/include/pci/raster/RasterCacheKey.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/raster_runtime/include/pci/runtime/raster/RasterTileCache.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/raster_runtime/src/RasterTileCache.cpp")
    list(APPEND violations
         "raster runtime: canonical cache-key/cache ownership split is missing")
endif()

foreach(raster_cache_relative_path IN ITEMS
        include/pci/runtime/raster/RasterTileCache.h
        src/RasterTileCache.cpp)
    set(raster_cache_path
        "${PROJECT_SOURCE_DIR}/src/raster_runtime/${raster_cache_relative_path}")
    if(EXISTS "${raster_cache_path}")
        file(STRINGS "${raster_cache_path}"
             raster_cache_upward_dependency
             REGEX "#[ \t]*include[ \t]*[<\"](scene/|renderer/|pci/document/|pci/operations/)")
        if(raster_cache_upward_dependency)
            list(APPEND violations
                 "raster_runtime/${raster_cache_relative_path}: decoded tile cache has an upward dependency")
        endif()
    endif()
endforeach()

foreach(scene_facade_header IN ITEMS
        DocumentUpdate.h
        RuntimeBudgetSnapshot.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${scene_facade_header}")
        list(APPEND violations
             "src/scene/${scene_facade_header}: compatibility-scene facade header remains")
    endif()
endforeach()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/desktop/session/include/pci/desktop/session/DocumentUpdate.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/runtime/scene/include/pci/runtime/scene/RuntimeBudgetSnapshot.h")
    list(APPEND violations
         "scene facade: canonical session publication/runtime-budget contracts are missing")
endif()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" compatibility_scene_include
         REGEX "#[ \t]*include[ \t]*[<\"]scene/")
    if(compatibility_scene_include)
        list(APPEND violations
             "${relative}: uses the retired compatibility-scene include tree")
    endif()
endforeach()

foreach(color_contract IN ITEMS
        CptColorMapParser.h
        PointColorMapCatalog.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/pointcloud/${color_contract}")
        list(APPEND violations
             "src/pointcloud/${color_contract}: shared color contract is owned by pointcloud")
    endif()
endforeach()

foreach(runtime_resource IN ITEMS
        PointMemoryBudget.h
        HierarchyResidencyCoordinator.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/${runtime_resource}")
        list(APPEND violations
             "src/scene/${runtime_resource}: shared runtime resource is owned by the scene facade")
    endif()
endforeach()

if(EXISTS "${PROJECT_SOURCE_DIR}/src/scene/RasterColorizeRunStore.h")
    list(APPEND violations
         "src/scene/RasterColorizeRunStore.h: run-store implementation remains coupled to the scene facade")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneDocumentSnapshot.h"
     scene_snapshot_document_include
     REGEX "SceneDocument\\.h")
if(scene_snapshot_document_include)
    list(APPEND violations
         "src/document/include/pci/document/SceneDocumentSnapshot.h: immutable snapshot declarations include the mutable document")
endif()

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneSnapshotLayer.h")
    list(APPEND violations
         "src/document/include/pci/document/SceneSnapshotLayer.h: metadata-only snapshot layer contract is missing")
endif()
if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/runtime/scene/include/pci/runtime/scene/RuntimeBudgetSnapshot.h")
    list(APPEND violations
         "runtime/scene: separate runtime budget snapshot is missing")
endif()
file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneDocumentSnapshot.h"
     document_snapshot_runtime_budget
     REGEX "decodedByteBudget")
if(document_snapshot_runtime_budget)
    list(APPEND violations
         "src/document/include/pci/document/SceneDocumentSnapshot.h: runtime budget remains embedded in document metadata")
endif()
file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneDocumentSnapshot.h"
     allocating_typed_snapshot_iteration
     REGEX "std::vector<(PointCloud|Vector|Raster)LayerSnapshot> (point|vector|raster)Layers\\(\\) const")
if(allocating_typed_snapshot_iteration)
    list(APPEND violations
         "src/document/include/pci/document/SceneDocumentSnapshot.h: typed snapshot iteration still materializes vectors")
endif()
file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneDocumentSnapshot.h"
     typed_snapshot_index_lists
     REGEX "(point|vector|raster)LayerIndices")
list(LENGTH typed_snapshot_index_lists typed_snapshot_index_list_count)
if(typed_snapshot_index_list_count LESS 3)
    list(APPEND violations
         "src/document/include/pci/document/SceneDocumentSnapshot.h: snapshot-local typed index lists are incomplete")
endif()
foreach(scene_snapshot_contract IN ITEMS
        document/include/pci/document/SceneDocumentSnapshot.h
        document/include/pci/document/SceneSnapshotLayer.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/${scene_snapshot_contract}")
        file(STRINGS "${PROJECT_SOURCE_DIR}/src/${scene_snapshot_contract}"
             scene_snapshot_runtime_reachability
             REGEX "(SceneLayer\\.h|PointCloudScenePtr|RasterLayerDataPtr|RasterTileSourcePtr|std::vector<SceneLayer>)")
        if(scene_snapshot_runtime_reachability)
            list(APPEND violations
                 "src/${scene_snapshot_contract}: immutable document snapshot exposes runtime ownership")
        endif()
    endif()
endforeach()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneLayer.h"
     document_owned_raster_payload
     REGEX "RasterLayerDataPtr data;")
if(document_owned_raster_payload)
    list(APPEND violations
         "src/document/include/pci/document/SceneLayer.h: live document layers still own raster runtime data")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneDocument.h"
     scene_document_raster_source_bridge
     REGEX "(RasterTileSource\\.h|RasterLayerDataPtr)")
if(scene_document_raster_source_bridge)
    list(APPEND violations
         "src/document/include/pci/document/SceneDocument.h: metadata-only document still exposes the raster source bridge")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneDocument.h"
     scene_document_runtime_resources
     REGEX "(DecodedPageCache|HierarchyResidencyCoordinator|PointMemoryBudget|TaskScheduler)")
if(scene_document_runtime_resources)
    list(APPEND violations
         "src/document/include/pci/document/SceneDocument.h: document still exposes point runtime resources")
endif()

foreach(point_document_file IN ITEMS
        document/include/pci/document/SceneDocument.h
        document/include/pci/document/SceneLayer.h)
    file(STRINGS "${PROJECT_SOURCE_DIR}/src/${point_document_file}"
         scene_document_point_runtime
         REGEX "PointCloudScene")
    if(scene_document_point_runtime)
        list(APPEND violations
             "src/${point_document_file}: metadata-only document still exposes a point runtime")
    endif()
endforeach()

file(STRINGS
     "${PROJECT_SOURCE_DIR}/src/runtime/scene/include/pci/runtime/scene/SceneRuntime.h"
     scene_runtime_document_include
     REGEX "SceneDocument\\.h")
if(scene_runtime_document_include)
    list(APPEND violations
         "runtime/scene/SceneRuntime.h: runtime bindings depend on the document model")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/rendering/frame/src/SceneSnapshotCache.cpp"
     snapshot_cache_embedded_runtime_access
     REGEX "point->scene")
if(snapshot_cache_embedded_runtime_access)
    list(APPEND violations
         "src/rendering/frame/src/SceneSnapshotCache.cpp: renderer cache reads runtime through the document snapshot")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/rendering/planning/include/pci/rendering/planning/RasterLodPlanner.h"
     raster_lod_document_contract
     REGEX "(#[ \t]*include[ \t]*[<\"]scene/|RasterLayer[ \t]+layer)")
if(raster_lod_document_contract)
    list(APPEND violations
         "src/rendering/planning/include/pci/rendering/planning/RasterLodPlanner.h: raster planning exposes a document layer instead of a metadata/style view")
endif()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/rendering/planning/include/pci/rendering/planning/RasterFrameCoordinator.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/rendering/planning/src/RasterFrameCoordinator.cpp")
    list(APPEND violations
         "renderer/planning: frame-global raster coordinator is missing")
endif()

foreach(raster_coordinator_source IN ITEMS
        renderer/planning/RasterFrameCoordinator.h
        renderer/planning/RasterFrameCoordinator.cpp)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/${raster_coordinator_source}")
        file(STRINGS "${PROJECT_SOURCE_DIR}/src/${raster_coordinator_source}"
             raster_coordinator_upward_dependency
             REGEX "#[ \t]*include[ \t]*[<\"](Q|QRhi|scene/)")
        if(raster_coordinator_upward_dependency)
            list(APPEND violations
                 "src/${raster_coordinator_source}: raster coordinator depends on Qt, QRhi, or the scene document")
        endif()
    endif()
endforeach()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/viewport/src/RenderViewportWidget.cpp"
     widget_owned_raster_planning
     REGEX "(planRasterTiles|previousRasterSelection_)")
if(widget_owned_raster_planning)
    list(APPEND violations
         "src/desktop/viewport/src/RenderViewportWidget.cpp: viewport still owns raster LOD planning state")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/rendering/planning/src/PointFrameCoordinator.cpp"
     point_planner_external_effects
     REGEX "scene->(requestNodes|trimDecodedCache)")
if(point_planner_external_effects)
    list(APPEND violations
         "src/rendering/planning/src/PointFrameCoordinator.cpp: point planning still schedules or evicts runtime data")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/rendering/planning/src/PointFrameCoordinator.cpp"
     point_planner_mutating_cache_lookups
     REGEX "->nodePayload\\(")
if(point_planner_mutating_cache_lookups)
    list(APPEND violations
         "src/rendering/planning/src/PointFrameCoordinator.cpp: point planning still mutates decoded-cache metrics or recency")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/rendering/planning/include/pci/rendering/planning/PointFrameCoordinator.h"
     point_planner_mutable_runtime_contracts
     REGEX "PointCloudLayer[ >]|PointCloudScenePtr|SceneDocumentSnapshot")
file(STRINGS "${PROJECT_SOURCE_DIR}/src/rendering/planning/src/PointFrameCoordinator.cpp"
     point_planner_mutable_runtime_access
     REGEX "layer\\.scene|scene->")
if(point_planner_mutable_runtime_contracts OR point_planner_mutable_runtime_access)
    list(APPEND violations
         "src/renderer/planning/PointFrameCoordinator: point planning still exposes a mutable scene/runtime contract")
endif()

foreach(point_executor_file IN ITEMS
        rendering/frame/include/pci/rendering/PointFrameExecutor.h
        rendering/frame/src/PointFrameExecutor.cpp)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/src/${point_executor_file}")
        list(APPEND violations
             "src/${point_executor_file}: point frame effects have no dedicated executor")
    endif()
endforeach()
file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/viewport/src/RenderViewportWidget.cpp"
     viewport_owned_point_runtime_effects
     REGEX "scene->(requestNodes|applyDecodedLookupEffect|trimDecodedCache)")
if(viewport_owned_point_runtime_effects)
    list(APPEND violations
         "src/desktop/viewport/src/RenderViewportWidget.cpp: viewport still executes point runtime effects directly")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/viewport/src/RenderViewportWidget.cpp"
     renderer_embedded_runtime_bridge
     REGEX "runtimeSnapshotFor|point->scene|raster->data")
file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/viewport/api/include/pci/desktop/viewport/RenderViewport.h"
     renderer_standalone_document_bridge
     REGEX "Transitional standalone bridge")
if(renderer_embedded_runtime_bridge OR renderer_standalone_document_bridge)
    list(APPEND violations
         "src/renderer: renderer still reconstructs runtime ownership from document-embedded handles")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/raster_runtime/include/pci/runtime/raster/RasterTileStreamer.h"
     raster_streamer_upward_includes
     REGEX "(SceneDocument|RasterLodPlanner)\\.h")
if(raster_streamer_upward_includes)
    list(APPEND violations
         "src/raster_runtime/include/pci/runtime/raster/RasterTileStreamer.h: raster runtime depends on document or planner types")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/raster_runtime/src/RasterTileStreamer.cpp"
     raster_streamer_document_access
     REGEX "(RasterLayer|RasterLodPlan|RasterFrameLayer|layer->data)")
if(raster_streamer_document_access)
    list(APPEND violations
         "src/raster_runtime/src/RasterTileStreamer.cpp: raster runtime consumes document or planner objects")
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/src/renderer/rhi/RasterTileStreamer.h" OR
   EXISTS "${PROJECT_SOURCE_DIR}/src/renderer/rhi/RasterTileStreamer.cpp")
    list(APPEND violations
         "src/renderer/rhi/RasterTileStreamer: portable raster runtime remains owned by the QRhi renderer")
endif()

if(NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/raster_runtime/include/pci/runtime/raster/RasterTileStreamer.h" OR
   NOT EXISTS
       "${PROJECT_SOURCE_DIR}/src/raster_runtime/src/RasterTileStreamer.cpp")
    list(APPEND violations
         "raster_runtime: canonical tile streamer implementation is missing")
endif()

set(raster_streamer_test
    "${PROJECT_SOURCE_DIR}/tests/raster_runtime/RasterTileStreamerTests.cpp")
if(NOT EXISTS "${raster_streamer_test}")
    list(APPEND violations
         "tests/raster_runtime: portable tile-streamer tests are missing")
else()
    file(STRINGS "${raster_streamer_test}"
         raster_streamer_test_upward_dependency
         REGEX "#[ \t]*include[ \t]*[<\"](Q|scene/|renderer/)")
    if(raster_streamer_test_upward_dependency)
        list(APPEND violations
             "tests/raster_runtime/RasterTileStreamerTests.cpp: runtime tests depend on Qt, scene, or renderer headers")
    endif()
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/tests/qt/RasterTileStreamerTests.cpp")
    list(APPEND violations
         "tests/qt/RasterTileStreamerTests.cpp: portable runtime tests remain in the Qt suite")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/session/include/pci/desktop/session/SceneSession.h"
     positional_document_update_flags
     REGEX "bool[ \t]+(frameVisibleLayers|replaceRendererDocument)")
if(positional_document_update_flags)
    list(APPEND violations
         "src/desktop/session/include/pci/desktop/session/SceneSession.h: document publication uses positional booleans instead of a typed DocumentUpdate")
endif()

file(GLOB_RECURSE desktop_app_sources
     "${PROJECT_SOURCE_DIR}/src/desktop/ui/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/desktop/ui/*.h")
foreach(source IN LISTS desktop_app_sources)
    get_filename_component(source_name "${source}" NAME)
    if(source_name MATCHES "^SceneSession\\.(cpp|h)$")
        continue()
    endif()
    file(STRINGS "${source}" mutable_session_document_access
         REGEX "session(_)?(->|\\.)document\\(")
    if(mutable_session_document_access)
        file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
        list(APPEND violations
             "${relative}: desktop consumers must use the immutable SceneSession document snapshot API")
    endif()
endforeach()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/session/include/pci/desktop/session/SceneSession.h"
     mutable_session_document_getter
     REGEX "SceneDocumentPtr[ \\t]+&[ \\t]*document\\(")
if(mutable_session_document_getter)
    list(APPEND violations
         "src/desktop/session/include/pci/desktop/session/SceneSession.h: SceneSession must not expose its mutable document owner")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneLayer.h"
     scene_binding_generation
     REGEX "BindingGeneration[ \t]+bindingGeneration")
file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/viewport/api/include/pci/desktop/viewport/RenderLoadProgress.h"
     render_progress_binding_generation
     REGEX "BindingGeneration[ \t]+bindingGeneration")
if(NOT scene_binding_generation OR NOT render_progress_binding_generation)
    list(APPEND violations
         "scene/render progress contracts: source bindings are not guarded by BindingGeneration")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneLayer.h"
     point_dataset_descriptor
     REGEX "PointDatasetDescriptor[ \t]+descriptor")
file(STRINGS "${PROJECT_SOURCE_DIR}/src/document/include/pci/document/SceneLayer.h"
     raster_dataset_descriptor
     REGEX "RasterDatasetDescriptor[ \t]+descriptor")
if(NOT point_dataset_descriptor OR NOT raster_dataset_descriptor)
    list(APPEND violations
         "src/document/include/pci/document/SceneLayer.h: layer publications lack immutable dataset descriptors")
endif()

foreach(legacy_operation_contract IN ITEMS
        "src/import/RasterImport.h"
        "src/import/PointCloudImport.h"
        "src/import/PointCloudStatistics.h"
        "src/vector/VectorImport.h")
    if(EXISTS "${PROJECT_SOURCE_DIR}/${legacy_operation_contract}")
        list(APPEND violations
             "${legacy_operation_contract}: portable provider contract is outside operations/api")
    endif()
endforeach()

if(NOT EXISTS
        "${PROJECT_SOURCE_DIR}/src/operations/api/include/pci/operations/PointCloudImport.h")
    list(APPEND violations
         "operations/api: portable point-import values are missing from their canonical module")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/src/import/PointCloudLoader.h")
    list(APPEND violations
         "import: runtime-valued point loader bridge must be removed")
endif()
foreach(point_provider IN ITEMS
        src/operations/api/include/pci/operations/PointCloudLoader.h
        src/operations/api/include/pci/operations/PreparedPointDataset.h
        src/adapters/pdal/include/pci/adapters/pdal/PdalPointCloudLoader.h
        src/adapters/pdal/src/PdalPointCloudLoader.cpp)
    file(STRINGS "${PROJECT_SOURCE_DIR}/${point_provider}" runtime_loader_values
         REGEX "PointDatasetRuntime|sceneReady|pci/runtime/point/")
    if(runtime_loader_values)
        list(APPEND violations "${point_provider}: point provider retains runtime-valued completion")
    endif()
endforeach()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" legacy_point_import_include
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]import/PointCloudImport\\.h[>\"]")
    if(legacy_point_import_include)
        list(APPEND violations
             "${relative}: uses the legacy mixed point-import contract")
    endif()
endforeach()

if(EXISTS
        "${PROJECT_SOURCE_DIR}/src/operations/api/include/pci/operations/PointCloudImport.h")
    file(STRINGS
         "${PROJECT_SOURCE_DIR}/src/operations/api/include/pci/operations/PointCloudImport.h"
         point_import_runtime_dependency
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](pci/runtime/point|pci/runtime/HierarchyResidencyCoordinator|pci/runtime/PointMemoryBudget|storage/)")
    if(point_import_runtime_dependency)
        list(APPEND violations
             "pci/operations/PointCloudImport.h: portable import values depend on runtime or storage implementation")
    endif()
endif()

foreach(legacy_foundation_header IN ITEMS
        Bounds3d.h
        CheckedArithmetic.h
        Hash.h
        SpatialReferenceComparator.h
        StrongId.h
        Vec3d.h)
    if(EXISTS
            "${PROJECT_SOURCE_DIR}/src/foundation/${legacy_foundation_header}")
        list(APPEND violations
             "src/foundation/${legacy_foundation_header}: public foundation header is outside its module include tree")
    endif()
endforeach()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" legacy_foundation_includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]foundation/")
    if(legacy_foundation_includes)
        list(APPEND violations
             "${relative}: uses a legacy foundation include path")
    endif()
endforeach()

foreach(legacy_point_header IN ITEMS
        GpuPoint.h
        GpuPointProperties.h
        PointAttributes.h
        PointClassificationFilter.h
        PointCloudMetadata.h
        PointColorPolicy.h
        SourcePoint.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/pointcloud/${legacy_point_header}")
        list(APPEND violations
             "src/pointcloud/${legacy_point_header}: public point-data header is outside its module include tree")
    endif()
endforeach()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" legacy_point_includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]pointcloud/")
    if(legacy_point_includes)
        list(APPEND violations
             "${relative}: uses a legacy point-data include path")
    endif()
endforeach()

foreach(legacy_raster_header IN ITEMS
        RasterLayer.h
        RasterPointSampler.h
        RasterTileCache.h
        RasterTileSource.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/raster/${legacy_raster_header}")
        list(APPEND violations
             "src/raster/${legacy_raster_header}: public raster header is outside its module include tree")
    endif()
endforeach()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" legacy_raster_includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]raster/")
    if(legacy_raster_includes)
        list(APPEND violations
             "${relative}: uses a legacy raster include path")
    endif()
endforeach()

foreach(legacy_vector_header IN ITEMS
        VectorGeometry.h
        VectorLayerData.h
        VectorLayerStyle.h)
    if(EXISTS "${PROJECT_SOURCE_DIR}/src/vector/${legacy_vector_header}")
        list(APPEND violations
             "src/vector/${legacy_vector_header}: public vector header is outside its module include tree")
    endif()
endforeach()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" legacy_vector_includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]vector/")
    if(legacy_vector_includes)
        list(APPEND violations
             "${relative}: uses a legacy vector include path")
    endif()
endforeach()

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    foreach(include IN LISTS includes)
        set(allowed FALSE)

        if(relative MATCHES "^src/rendering/planning/" AND
           include MATCHES "[<\"](pci/runtime/|pci/desktop/|pci/rendering/rhi/)")
            list(APPEND violations
                 "${relative}: planner includes an execution or desktop contract")
        endif()

        if(relative MATCHES "^src/rendering/rhi/" AND
           include MATCHES "[<\"](pci/desktop/|QWidget|QRhiWidget|QtWidgets)")
            list(APPEND violations
                 "${relative}: renderer includes a desktop viewport contract")
        endif()

        if(include MATCHES "[<\"]pdal/")
            if(relative MATCHES "^src/adapters/pdal/" OR
               relative MATCHES "^tests/fixtures/Pdal" OR
               relative STREQUAL "tests/component/PdalImportTests.cpp")
                set(allowed TRUE)
            endif()
            if(NOT allowed)
                list(APPEND violations
                     "${relative}: PDAL include is outside the PDAL adapter")
            endif()
        endif()

        if(include MATCHES
                "[<\"](gdal[^/]*|ogr[^/]*|cpl_[^/]*)\\.h[>\"]")
            set(allowed FALSE)
            if(relative MATCHES "^src/adapters/gdal/" OR
               relative MATCHES "^src/adapters/ogr/" OR
               relative MATCHES "^tests/fixtures/(Gdal|Ogr)" OR
               relative STREQUAL
                   "tests/component/OgrVectorImportTests.cpp" OR
               relative STREQUAL
                   "tests/component/GdalRasterImportTests.cpp" OR
               relative STREQUAL
                   "tests/component/GdalRuntimeTests.cpp" OR
               relative STREQUAL
                   "tests/stress/RasterLargeSourceTests.cpp")
                set(allowed TRUE)
            endif()
            if(NOT allowed)
                list(APPEND violations
                     "${relative}: GDAL/OGR include is outside GDAL/OGR adapters and approved tests")
            endif()
        endif()

        if(include MATCHES
                "[<\"](QRhi[^>\"]*|rhi/[^>\"]*|pci/rendering/rhi/[^>\"]*)[>\"]")
            set(allowed FALSE)
            if(relative MATCHES "^src/rendering/rhi/" OR
               relative MATCHES "^src/desktop/viewport/" OR
               relative MATCHES "^tests/(gpu|renderer_internal)/" OR
               relative MATCHES
                   "^tests/qt/(EmbeddedColorMap|BackendPolicy|PointCloudRenderer|VectorLayerRenderer|RasterLayerRenderer|UploadScheduler|PointPickResult)Tests.cpp$" OR
               relative STREQUAL
                   "tests/support/RenderViewportTestAccess.h" OR
               # The large-source acceptance lane is cross-target on purpose:
               # the bound it asserts spans the GDAL adapter, which issues the
               # reads, and the streamer, which admits and evicts them. Testing
               # either half alone could not show that reads stay proportional
               # to requested tiles rather than to source size.
               relative STREQUAL
                   "tests/stress/RasterLargeSourceTests.cpp")
                set(allowed TRUE)
            endif()
            if(NOT allowed)
                list(APPEND violations
                     "${relative}: QRhi include is outside renderer internals")
            endif()
        endif()

        if(relative MATCHES
                "^src/(foundation|color|pointcloud|raster|vector|navigation|document|runtime|raster_runtime|tasking|runtime_resources|operations/(api|jobs|import|colorize)|rendering/telemetry|rendering/frame)/" OR
           relative MATCHES "^src/rendering/planning/")
            if(include MATCHES "[<\"](Q[A-Z][^>\"]*|Qt[^>\"]*)[>\"]")
                list(APPEND violations
                     "${relative}: portable target includes Qt header")
            endif()
        endif()
    endforeach()
endforeach()


# P5: orchestration is portable; desktop classes only adapt callbacks and widgets.
file(GLOB_RECURSE portable_jobs "${PROJECT_SOURCE_DIR}/src/operations/jobs/*.h" "${PROJECT_SOURCE_DIR}/src/operations/jobs/*.cpp" "${PROJECT_SOURCE_DIR}/src/operations/import/*.h" "${PROJECT_SOURCE_DIR}/src/operations/import/*.cpp")
foreach(source IN LISTS portable_jobs)
    file(STRINGS "${source}" upward_includes REGEX "#[ \t]*include[ \t]*[<\"](pci/desktop/|pci/adapters/|pci/document/)")
    if(upward_includes)
        list(APPEND violations "${source}: portable operation depends on desktop/document orchestration")
    endif()
endforeach()
file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/ui/src/PointCloudStatisticsDialog.cpp" dialog_workers REGEX "QtConcurrent|QFuture|std::thread|jthread|calculate\\(")
if(dialog_workers)
    list(APPEND violations "statistics dialog owns a worker instead of a scoped operation subscription")
endif()
file(STRINGS "${PROJECT_SOURCE_DIR}/src/desktop/session/src/SceneSession.cpp" legacy_task_routing REGEX "case LoadJobKind::|controller.*jobRows\\(")
if(legacy_task_routing)
    list(APPEND violations "SceneSession bypasses operation registry task routing")
endif()

# Final P6 boundaries apply to application and production tools as well.
foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" legacy_includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](app|desktop|import|renderer|platform|storage)/")
    if(legacy_includes)
        list(APPEND violations "${relative}: legacy global-source include spelling")
    endif()
    if(relative MATCHES "/include/")
        file(STRINGS "${source}" private_escape REGEX "#[ \t]*include.*\\.\\./")
        if(private_escape)
            list(APPEND violations "${relative}: public header reaches a private parent directory")
        endif()
    endif()
endforeach()
file(STRINGS "${PROJECT_SOURCE_DIR}/src/CMakeLists.txt" root_libraries REGEX "^add_library\\(")
if(root_libraries)
    list(APPEND violations "src/CMakeLists.txt: module target remains in executable composition")
endif()

if(violations)
    list(JOIN violations "\n  " formatted)
    message(FATAL_ERROR "Include boundary violations:\n  ${formatted}")
endif()

message(STATUS "Project include boundaries are valid")
