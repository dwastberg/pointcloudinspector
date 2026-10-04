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

foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(READ "${source}" content)
    file(STRINGS "${source}" includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    foreach(include IN LISTS includes)
        set(allowed FALSE)

        if(include MATCHES "[<\"](app|desktop|import|renderer|platform|storage|scene|foundation|pointcloud|raster|vector|navigation|tasking)/")
            list(APPEND violations "${relative}: global-source include spelling; use the owning module's public include tree")
        endif()
        if(relative MATCHES "/include/" AND include MATCHES "\\.\\./")
            list(APPEND violations "${relative}: public header reaches a private parent directory")
        endif()

        if(relative MATCHES "^src/runtime/(point|scene)/" AND
           include MATCHES "[<\"]pci/(document|operations|desktop)/")
            list(APPEND violations "${relative}: runtime depends on document or operation orchestration")
        endif()
        if(relative MATCHES "^src/raster/" AND
           include MATCHES "[<\"]pci/(document|runtime|operations|desktop)/")
            list(APPEND violations "${relative}: raster data has an upward dependency")
        endif()
        if(relative MATCHES "^src/raster_runtime/" AND
           include MATCHES "[<\"]pci/(document|operations|rendering|desktop)/")
            list(APPEND violations "${relative}: raster runtime has an upward dependency")
        endif()
        if(relative MATCHES "^src/document/" AND
           include MATCHES "[<\"]pci/(runtime|operations|adapters|desktop)/")
            list(APPEND violations "${relative}: metadata-only document includes runtime or orchestration")
        endif()
        if(relative MATCHES "^src/operations/(api|jobs|import)/" AND
           include MATCHES "[<\"]pci/(desktop|adapters|document)/")
            list(APPEND violations "${relative}: portable operation depends on desktop, adapters, or document")
        endif()
        if(relative MATCHES "^src/operations/api/" AND
           include MATCHES "[<\"]pci/runtime/(point|scene|raster)/")
            list(APPEND violations "${relative}: portable operation values depend on mutable runtime")
        endif()
        if(relative MATCHES "^tests/raster_runtime/" AND
           include MATCHES "[<\"](Q|pci/(document|rendering|desktop)/)")
            list(APPEND violations "${relative}: portable raster runtime test has an upward dependency")
        endif()

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

    # Ownership constraints that also catch forward declarations and access
    # through another contract. Functional behavior belongs in C++ tests.
    if(relative MATCHES "^src/document/.*\\.h$" AND
       content MATCHES "PointDatasetRuntime([^A-Za-z0-9_]|$)|RasterLayerDataPtr|RasterTileSourcePtr|DecodedPageCache|HierarchyResidencyCoordinator|PointMemoryBudget|TaskScheduler")
        list(APPEND violations "${relative}: metadata-only document exposes runtime ownership")
    endif()
    if(relative MATCHES "^src/document/.*(SceneDocumentSnapshot|SceneSnapshotLayer)\\.h$" AND
       content MATCHES "SceneDocument\\.h|SceneLayer\\.h|decodedByteBudget")
        list(APPEND violations "${relative}: immutable snapshot exposes mutable document or runtime state")
    endif()
    if(relative MATCHES "^src/rendering/planning/" AND
       content MATCHES "PointDatasetRuntime([^A-Za-z0-9_]|$)|->(requestNodes|trimDecodedCache|nodePayload)\\(")
        list(APPEND violations "${relative}: planner accesses mutable runtime")
    endif()
    if(relative MATCHES "^src/rendering/planning/.*PointFrameCoordinator\\.h$" AND
       content MATCHES "PointCloudLayer[ >]|SceneDocumentSnapshot")
        list(APPEND violations "${relative}: point planner exposes a document instead of immutable point data")
    endif()
    if(relative MATCHES "^src/rendering/planning/.*RasterLodPlanner\\.h$" AND
       content MATCHES "RasterLayer[ \t]+layer")
        list(APPEND violations "${relative}: raster planner exposes a document layer instead of metadata and style")
    endif()
    if(relative MATCHES "^src/raster_runtime/" AND
       content MATCHES "RasterLayer[ >]|RasterLodPlan[ >]|RasterFrameLayer[ >]|layer->data")
        list(APPEND violations "${relative}: raster runtime consumes document or planner objects")
    endif()
    if(relative MATCHES "^src/rendering/frame/.*SceneSnapshotCache\\.cpp$" AND
       content MATCHES "point->scene")
        list(APPEND violations "${relative}: frame cache reads runtime ownership through document metadata")
    endif()
    if(relative MATCHES "^src/operations/api/" AND
       content MATCHES "PointDatasetRuntime([^A-Za-z0-9_]|$)|sceneReady")
        list(APPEND violations "${relative}: portable operation exposes runtime-valued completion")
    endif()
    if(relative MATCHES "^src/adapters/pdal/.*PdalPointCloudLoader\\.(cpp|h)$" AND
       content MATCHES "PointDatasetRuntime([^A-Za-z0-9_]|$)|sceneReady|pci/runtime/point/")
        list(APPEND violations "${relative}: point provider exposes runtime-valued completion")
    endif()
    if(relative MATCHES "^src/operations/api/.*RasterPointColorize\\.h$" AND
       content MATCHES "RasterPointColorApplyOutcome|RasterPointColorizeAvailability|colorizedSource|struct Raster(ColorizeHierarchicalTarget|ColorizeFlatTarget|ColorizeTargetSnapshot|ColorizeRange)")
        list(APPEND violations "${relative}: operation API owns point data or runtime installation state")
    endif()
    if(relative MATCHES "^src/desktop/ui/" AND
       content MATCHES "session(_)?(->|\\.)document\\(")
        list(APPEND violations "${relative}: desktop consumers must use the immutable session snapshot")
    endif()
    if(relative MATCHES "^src/desktop/session/.*SceneSession\\.h$" AND
       content MATCHES "SceneDocumentPtr[ \t]+&[ \t]*document\\(")
        list(APPEND violations "${relative}: session exposes its mutable document owner")
    endif()
    if(relative MATCHES "^src/desktop/viewport/.*RenderViewportWidget\\.cpp$" AND
       content MATCHES "planRasterTiles|previousRasterSelection_|scene->(requestNodes|applyDecodedLookupEffect|trimDecodedCache)|runtimeSnapshotFor|point->scene|raster->data")
        list(APPEND violations "${relative}: viewport owns planning or runtime effects belonging to frame execution")
    endif()
    if(relative MATCHES "^src/desktop/ui/.*PointCloudStatisticsDialog\\.cpp$" AND
       content MATCHES "QtConcurrent|QFuture|std::thread|jthread|calculate\\(")
        list(APPEND violations "${relative}: statistics dialog owns an operation worker")
    endif()
    if(relative MATCHES "^src/desktop/session/.*SceneSession\\.cpp$" AND
       content MATCHES "case LoadJobKind::|controller.*jobRows\\(")
        list(APPEND violations "${relative}: session bypasses operation registry task routing")
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n  " formatted)
    message(FATAL_ERROR "Include boundary violations:\n  ${formatted}")
endif()

message(STATUS "Project include boundaries are valid")
