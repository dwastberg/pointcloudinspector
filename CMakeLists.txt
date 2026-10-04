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
include(GNUInstallDirs)

include(cmake/PciOptions.cmake)

find_package(PDAL 2.10 REQUIRED CONFIG)
# Use GDAL's config package where available and CMake's FindGDAL fallback elsewhere.
find_package(GDAL 3.9 REQUIRED)

# QRhi is provided through Qt's GuiPrivate target. The dependency is deliberate
# and means the application must run with the same Qt build it was compiled
# against; suppress Qt's generic warning after making that constraint explicit.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
set(pcinspector_qt_components
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

# Qt's standard setup enables autogen globally. Targets using Q_OBJECT opt in
# explicitly, avoiding moc/uic work for portable libraries.
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
include(cmake/PciHeaderSelfContainment.cmake)

include(cmake/PciFormatting.cmake)
include(cmake/PciModules.cmake)

add_subdirectory(src)
if(PCINSPECTOR_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
    pci_register_architecture_checks()
endif()

include(cmake/PciInstall.cmake)
