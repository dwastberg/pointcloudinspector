cmake_minimum_required(VERSION 3.24...4.2)

project(pcinspector
        VERSION 0.1.0
        DESCRIPTION "Cross-platform point-cloud inspector"
        LANGUAGES CXX)

# This project does not use C++ modules. CMake 3.28 and newer may otherwise
# scan every C++20-or-newer source for module dependencies, which needlessly
# requires compiler-specific scanning tools such as clang-scan-deps.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(PCINSPECTOR_APPLICATION_ID
    "io.github.dwastberg.PointCloudInspector")
set(PCINSPECTOR_APPLICATION_NAME "Point Cloud Inspector")
file(GENERATE
     OUTPUT "${CMAKE_BINARY_DIR}/pcinspector-version.txt"
     CONTENT "${PROJECT_VERSION}\n")

if(WIN32)
    enable_language(RC)
endif()

include(cmake/PciToolchain.cmake)
include(CTest)
include(CMakeDependentOption)
include(GNUInstallDirs)

option(PCINSPECTOR_BUILD_TOOLS
       "Build developer profiling and diagnostic tools" OFF)
option(PCINSPECTOR_ENABLE_DIAGNOSTIC_UI
       "Expose profiling, residency, and renderer diagnostics in the UI" OFF)
option(PCINSPECTOR_WARNINGS_AS_ERRORS
       "Treat project compiler warnings as errors" OFF)
option(PCINSPECTOR_DEPLOY_RUNTIME_DEPENDENCIES
       "Deploy Qt runtime dependencies when installing the application" OFF)
if(APPLE)
    set(pcinspector_default_gpu_test_api metal)
elseif(WIN32)
    set(pcinspector_default_gpu_test_api d3d12)
else()
    set(pcinspector_default_gpu_test_api vulkan)
endif()
set(PCINSPECTOR_GPU_TEST_GRAPHICS_API
    "${pcinspector_default_gpu_test_api}" CACHE STRING
    "Graphics API for the opt-in native GPU test lane")
set_property(CACHE PCINSPECTOR_GPU_TEST_GRAPHICS_API PROPERTY STRINGS
             auto metal vulkan d3d11 d3d12 opengl)
unset(pcinspector_default_gpu_test_api)
option(PCINSPECTOR_GPU_TEST_VALIDATION
       "Enable the backend validation/debug layer in native GPU tests" ON)
cmake_dependent_option(
        PCINSPECTOR_ENABLE_GPU_TESTS
        "Build and register tests that require a native GPU device"
        OFF
        "BUILD_TESTING"
        OFF)

set(pcinspector_valid_gpu_test_apis
    auto metal vulkan d3d11 d3d12 opengl)
if(NOT PCINSPECTOR_GPU_TEST_GRAPHICS_API
        IN_LIST pcinspector_valid_gpu_test_apis)
    message(FATAL_ERROR
            "PCINSPECTOR_GPU_TEST_GRAPHICS_API must be one of: ${pcinspector_valid_gpu_test_apis}")
endif()
if(PCINSPECTOR_GPU_TEST_GRAPHICS_API STREQUAL "metal" AND NOT APPLE)
    message(FATAL_ERROR "The Metal GPU test lane requires Apple platforms")
endif()
if(PCINSPECTOR_GPU_TEST_GRAPHICS_API MATCHES "^d3d(11|12)$" AND NOT WIN32)
    message(FATAL_ERROR "Direct3D GPU test lanes require Windows")
endif()
unset(pcinspector_valid_gpu_test_apis)

find_package(PDAL 2.10 REQUIRED CONFIG)
# Use GDAL's config package where available and CMake's FindGDAL fallback elsewhere.
find_package(GDAL 3.9 REQUIRED)

# QRhi is provided through Qt's GuiPrivate target. The dependency is deliberate
# and means the application must run with the same Qt build it was compiled
# against; suppress Qt's generic warning after making that constraint explicit.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
set(pcinspector_qt_components
        Concurrent
        Core
        Gui
        GuiPrivate
        ShaderTools
        Widgets)
if(BUILD_TESTING)
    list(APPEND pcinspector_qt_components Test)
endif()
find_package(Qt6 6.7 REQUIRED COMPONENTS ${pcinspector_qt_components})
unset(pcinspector_qt_components)

qt_standard_project_setup(REQUIRES 6.7)

# Qt's standard setup enables autogen globally. Only two project targets use
# Q_OBJECT, so opt in on those targets rather than running moc/uic everywhere.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)

# PDAL 2.10 exports `pdalcpp` without a namespace. Normalize that target while
# also accepting packages that already provide the conventional namespaced
# spelling. A target is required so multi-config library locations and usage
# requirements remain the responsibility of the PDAL package.
if(NOT TARGET PDAL::pdalcpp)
    if(TARGET pdalcpp)
        add_library(PDAL::pdalcpp ALIAS pdalcpp)
    else()
        message(FATAL_ERROR
                "PDAL ${PDAL_VERSION} did not export a pdalcpp CMake target")
    endif()
endif()

include(cmake/PciProjectOptions.cmake)
include(cmake/PciColorMaps.cmake)
include(cmake/PciShaders.cmake)
include(cmake/PciArchitecture.cmake)

find_program(PCINSPECTOR_CLANG_FORMAT_EXECUTABLE
             NAMES clang-format
             DOC "clang-format executable used by format targets")
if(PCINSPECTOR_CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE pcinspector_format_sources
         CONFIGURE_DEPENDS
         "${PROJECT_SOURCE_DIR}/src/*.cpp"
         "${PROJECT_SOURCE_DIR}/src/*.h"
         "${PROJECT_SOURCE_DIR}/tests/*.cpp"
         "${PROJECT_SOURCE_DIR}/tests/*.h"
         "${PROJECT_SOURCE_DIR}/tools/*.cpp"
         "${PROJECT_SOURCE_DIR}/tools/*.h")
    list(APPEND pcinspector_format_sources "${PROJECT_SOURCE_DIR}/main.cpp")

    add_custom_target(format
        COMMAND "${PCINSPECTOR_CLANG_FORMAT_EXECUTABLE}"
                -i
                ${pcinspector_format_sources}
        COMMENT "Formatting Point Cloud Inspector C++ sources"
        VERBATIM)
    add_custom_target(format-check
        COMMAND "${PCINSPECTOR_CLANG_FORMAT_EXECUTABLE}"
                --dry-run
                --Werror
                ${pcinspector_format_sources}
        COMMENT "Checking Point Cloud Inspector C++ formatting"
        VERBATIM)
else()
    message(STATUS
            "clang-format not found; format and format-check targets are unavailable")
endif()
unset(pcinspector_format_sources)

add_subdirectory(src)
if(PCINSPECTOR_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
    pci_register_architecture_checks()
endif()

if(WIN32 AND PCINSPECTOR_DEPLOY_RUNTIME_DEPENDENCIES)
    install(TARGETS pcinspector
            RUNTIME_DEPENDENCY_SET pcinspector_runtime_dependencies
            BUNDLE DESTINATION .
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime)
    install(RUNTIME_DEPENDENCY_SET pcinspector_runtime_dependencies
            DIRECTORIES
                "$ENV{CONDA_PREFIX}/Library/bin"
                "$ENV{CONDA_PREFIX}/bin"
            PRE_EXCLUDE_REGEXES
                "api-ms-.*"
                "ext-ms-.*"
                "Qt6.*\\.dll"
            POST_EXCLUDE_REGEXES
                ".*[Ww]indows[/\\\\][Ss]ystem32[/\\\\].*"
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime)
else()
    install(TARGETS pcinspector
            BUNDLE DESTINATION .
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime)
endif()
install(FILES README.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        COMPONENT Runtime)

if(UNIX AND NOT APPLE)
    set(pcinspector_linux_icon_sizes
            16 24 32 48 64 96 128 256 512)
    foreach(pcinspector_icon_size IN LISTS pcinspector_linux_icon_sizes)
        install(FILES
                "${PROJECT_SOURCE_DIR}/assets/icons/platform/linux/hicolor/${pcinspector_icon_size}x${pcinspector_icon_size}/apps/pcinspector.png"
                DESTINATION
                "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${pcinspector_icon_size}x${pcinspector_icon_size}/apps"
                RENAME "${PCINSPECTOR_APPLICATION_ID}.png"
                COMPONENT Runtime)
    endforeach()
    unset(pcinspector_linux_icon_sizes)
    unset(pcinspector_icon_size)
    install(FILES
            "${PROJECT_SOURCE_DIR}/assets/icons/platform/linux/hicolor/scalable/apps/pcinspector.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
            RENAME "${PCINSPECTOR_APPLICATION_ID}.svg"
            COMPONENT Runtime)
    install(FILES
            "${PROJECT_SOURCE_DIR}/assets/icons/platform/linux/${PCINSPECTOR_APPLICATION_ID}.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
            COMPONENT Runtime)
    configure_file(
            "${PROJECT_SOURCE_DIR}/assets/icons/platform/linux/${PCINSPECTOR_APPLICATION_ID}.metainfo.xml.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${PCINSPECTOR_APPLICATION_ID}.metainfo.xml"
            @ONLY)
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/${PCINSPECTOR_APPLICATION_ID}.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
            COMPONENT Runtime)
endif()

if(PCINSPECTOR_DEPLOY_RUNTIME_DEPENDENCIES)
    set(PCINSPECTOR_GDAL_DATA_DIR "$ENV{GDAL_DATA}" CACHE PATH
        "Directory containing the GDAL runtime data files")
    set(PCINSPECTOR_PROJ_DATA_DIR "$ENV{PROJ_DATA}" CACHE PATH
        "Directory containing the PROJ runtime data files")

    if(NOT PCINSPECTOR_GDAL_DATA_DIR)
        find_path(pcinspector_detected_gdal_data_dir
                  NAMES gdalvrt.xsd
                  HINTS
                      "$ENV{CONDA_PREFIX}/share/gdal"
                      "$ENV{CONDA_PREFIX}/Library/share/gdal"
                  PATH_SUFFIXES share/gdal)
        if(pcinspector_detected_gdal_data_dir)
            set(PCINSPECTOR_GDAL_DATA_DIR
                "${pcinspector_detected_gdal_data_dir}" CACHE PATH
                "Directory containing the GDAL runtime data files" FORCE)
        endif()
    endif()
    if(NOT PCINSPECTOR_PROJ_DATA_DIR)
        find_path(pcinspector_detected_proj_data_dir
                  NAMES proj.db
                  HINTS
                      "$ENV{CONDA_PREFIX}/share/proj"
                      "$ENV{CONDA_PREFIX}/Library/share/proj"
                  PATH_SUFFIXES share/proj)
        if(pcinspector_detected_proj_data_dir)
            set(PCINSPECTOR_PROJ_DATA_DIR
                "${pcinspector_detected_proj_data_dir}" CACHE PATH
                "Directory containing the PROJ runtime data files" FORCE)
        endif()
    endif()

    if(NOT IS_DIRECTORY "${PCINSPECTOR_GDAL_DATA_DIR}")
        message(FATAL_ERROR
                "Packaging requires PCINSPECTOR_GDAL_DATA_DIR to identify the GDAL data directory")
    endif()
    if(NOT IS_DIRECTORY "${PCINSPECTOR_PROJ_DATA_DIR}")
        message(FATAL_ERROR
                "Packaging requires PCINSPECTOR_PROJ_DATA_DIR to identify the PROJ data directory")
    endif()

    if(APPLE)
        set(pcinspector_runtime_data_destination
            "${PCINSPECTOR_APPLICATION_NAME}.app/Contents/Resources")
    else()
        set(pcinspector_runtime_data_destination "${CMAKE_INSTALL_DATADIR}")
    endif()
    file(REAL_PATH "${PCINSPECTOR_GDAL_DATA_DIR}"
         pcinspector_resolved_gdal_data_dir)
    file(REAL_PATH "${PCINSPECTOR_PROJ_DATA_DIR}"
         pcinspector_resolved_proj_data_dir)
    install(DIRECTORY "${pcinspector_resolved_gdal_data_dir}/"
            DESTINATION "${pcinspector_runtime_data_destination}/gdal"
            COMPONENT Runtime)
    install(DIRECTORY "${pcinspector_resolved_proj_data_dir}/"
            DESTINATION "${pcinspector_runtime_data_destination}/proj"
            COMPONENT Runtime)
endif()

if(PCINSPECTOR_DEPLOY_RUNTIME_DEPENDENCIES)
    set(pcinspector_qt_deploy_arguments)
    if(APPLE)
        set(pcinspector_macos_plugin_root
            "${PCINSPECTOR_APPLICATION_NAME}.app/Contents/PlugIns")
        foreach(pcinspector_plugin IN ITEMS
                "Qt6::QCocoaIntegrationPlugin|platforms"
                "Qt6::QMacStylePlugin|styles"
                "Qt6::QGifPlugin|imageformats"
                "Qt6::QICNSPlugin|imageformats"
                "Qt6::QICOPlugin|imageformats"
                "Qt6::QJpegPlugin|imageformats")
            string(REPLACE "|" ";" pcinspector_plugin_fields
                   "${pcinspector_plugin}")
            list(GET pcinspector_plugin_fields 0 pcinspector_plugin_target)
            list(GET pcinspector_plugin_fields 1 pcinspector_plugin_type)
            if(NOT TARGET ${pcinspector_plugin_target})
                message(FATAL_ERROR
                        "Required macOS Qt plugin is unavailable: ${pcinspector_plugin_target}")
            endif()
            set(pcinspector_plugin_location)
            foreach(pcinspector_plugin_location_property IN ITEMS
                    IMPORTED_LOCATION_RELEASE
                    IMPORTED_LOCATION_RELWITHDEBINFO
                    IMPORTED_LOCATION)
                get_target_property(pcinspector_candidate_plugin_location
                    ${pcinspector_plugin_target}
                    ${pcinspector_plugin_location_property})
                if(pcinspector_candidate_plugin_location AND
                        NOT pcinspector_candidate_plugin_location MATCHES
                            "-NOTFOUND$")
                    set(pcinspector_plugin_location
                        "${pcinspector_candidate_plugin_location}")
                    break()
                endif()
            endforeach()
            if(NOT pcinspector_plugin_location)
                message(FATAL_ERROR
                        "Required macOS Qt plugin has no release location: ${pcinspector_plugin_target}")
            endif()
            get_filename_component(pcinspector_plugin_filename
                "${pcinspector_plugin_location}" NAME)
            file(REAL_PATH "${pcinspector_plugin_location}"
                 pcinspector_resolved_plugin_location)
            install(FILES "${pcinspector_resolved_plugin_location}"
                    DESTINATION
                        "${pcinspector_macos_plugin_root}/${pcinspector_plugin_type}"
                    RENAME "${pcinspector_plugin_filename}"
                    COMPONENT Runtime)
        endforeach()
        # macdeployqt otherwise discovers unrelated plugins from a full Qt
        # installation. The required plugins are installed explicitly above.
        list(APPEND pcinspector_qt_deploy_arguments NO_PLUGINS)
    endif()

    qt_generate_deploy_app_script(
            TARGET pcinspector
            OUTPUT_SCRIPT pcinspector_deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR
            ${pcinspector_qt_deploy_arguments})
    install(SCRIPT ${pcinspector_deploy_script}
            COMPONENT Runtime)

    if(APPLE)
        set(pcinspector_dependency_search_dirs)
        if(NOT "$ENV{CONDA_PREFIX}" STREQUAL "")
            list(APPEND pcinspector_dependency_search_dirs
                 "$ENV{CONDA_PREFIX}/lib"
                 "$ENV{CONDA_PREFIX}/Library/lib")
        endif()
        foreach(pcinspector_dependency_target IN ITEMS
                PDAL::pdalcpp GDAL::GDAL)
            foreach(pcinspector_location_property IN ITEMS
                    IMPORTED_LOCATION_RELEASE
                    IMPORTED_LOCATION_RELWITHDEBINFO
                    IMPORTED_LOCATION)
                get_target_property(pcinspector_dependency_location
                    ${pcinspector_dependency_target}
                    ${pcinspector_location_property})
                if(pcinspector_dependency_location AND
                        NOT pcinspector_dependency_location MATCHES "-NOTFOUND$")
                    get_filename_component(pcinspector_dependency_directory
                        "${pcinspector_dependency_location}" DIRECTORY)
                    list(APPEND pcinspector_dependency_search_dirs
                         "${pcinspector_dependency_directory}")
                    break()
                endif()
            endforeach()
        endforeach()
        list(REMOVE_DUPLICATES pcinspector_dependency_search_dirs)
        configure_file(
                "${PROJECT_SOURCE_DIR}/cmake/FixupMacBundle.cmake.in"
                "${CMAKE_CURRENT_BINARY_DIR}/FixupMacBundle.cmake"
                @ONLY)
        install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/FixupMacBundle.cmake"
                COMPONENT Runtime)
    endif()
endif()

if(APPLE OR WIN32)
    include(cmake/PciPackaging.cmake)
endif()
