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
     "${PROJECT_SOURCE_DIR}/tests/*.h")
list(APPEND project_sources "${PROJECT_SOURCE_DIR}/main.cpp")

set(violations "")
foreach(source IN LISTS project_sources)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    file(STRINGS "${source}" includes
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    foreach(include IN LISTS includes)
        set(allowed FALSE)

        if(include MATCHES "[<\"]pdal/")
            if(relative MATCHES "^src/import/pdal/" OR
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
            if(relative MATCHES "^src/import/gdal/" OR
               relative MATCHES "^src/import/ogr/" OR
               relative MATCHES "^tests/fixtures/(Gdal|Ogr)" OR
               relative STREQUAL
                   "tests/component/OgrVectorImportTests.cpp" OR
               relative STREQUAL
                   "tests/component/GdalRasterImportTests.cpp")
                set(allowed TRUE)
            endif()
            if(NOT allowed)
                list(APPEND violations
                     "${relative}: GDAL/OGR include is outside the OGR adapter")
            endif()
        endif()

        if(include MATCHES
                "[<\"](QRhi[^>\"]*|rhi/[^>\"]*|renderer/rhi/[^>\"]*)[>\"]")
            set(allowed FALSE)
            if(relative MATCHES "^src/renderer/rhi/" OR
               relative MATCHES "^tests/(gpu|renderer_internal)/" OR
               relative MATCHES
                   "^tests/qt/(BackendPolicy|PointCloudRenderer|VectorLayerRenderer|RasterLayerRenderer|RasterTileStreamer|UploadScheduler|PointPickResult)Tests.cpp$" OR
               relative STREQUAL
                   "tests/support/RenderViewportTestAccess.h")
                set(allowed TRUE)
            endif()
            if(NOT allowed)
                list(APPEND violations
                     "${relative}: QRhi include is outside renderer internals")
            endif()
        endif()

        if(relative MATCHES
                "^src/(foundation|pointcloud|raster|vector|tasking)/" OR
           relative MATCHES "^src/renderer/planning/")
            if(include MATCHES "[<\"](Q[A-Z][^>\"]*|Qt[^>\"]*)[>\"]")
                list(APPEND violations
                     "${relative}: portable target includes Qt header")
            endif()
        endif()
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n  " formatted)
    message(FATAL_ERROR "Include boundary violations:\n  ${formatted}")
endif()

message(STATUS "Project include boundaries are valid")
