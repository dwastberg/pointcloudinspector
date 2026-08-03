cmake_minimum_required(VERSION 3.24...4.2)

project(pcinspector
        VERSION 0.1.0
        DESCRIPTION "Cross-platform point-cloud inspector"
        LANGUAGES CXX)

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
find_package(GDAL 3.4 REQUIRED)

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

install(TARGETS pcinspector
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT Runtime)
install(FILES README.md
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
                COMPONENT Runtime)
    endforeach()
    unset(pcinspector_linux_icon_sizes)
    unset(pcinspector_icon_size)
    install(FILES
            "${PROJECT_SOURCE_DIR}/assets/icons/platform/linux/hicolor/scalable/apps/pcinspector.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
            COMPONENT Runtime)
    install(FILES
            "${PROJECT_SOURCE_DIR}/assets/icons/platform/linux/pcinspector.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
            COMPONENT Runtime)
endif()

if(PCINSPECTOR_DEPLOY_RUNTIME_DEPENDENCIES)
    qt_generate_deploy_app_script(
            TARGET pcinspector
            OUTPUT_SCRIPT pcinspector_deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${pcinspector_deploy_script}
            COMPONENT Runtime)
endif()
