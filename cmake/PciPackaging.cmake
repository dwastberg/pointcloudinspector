set(CPACK_PACKAGE_NAME "PointCloudInspector")
set(CPACK_PACKAGE_VENDOR "Point Cloud Inspector Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Cross-platform point-cloud inspector")
set(CPACK_PACKAGE_HOMEPAGE_URL
    "https://github.com/dwastberg/pointcloudinspector")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Point Cloud Inspector")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_MONOLITHIC_INSTALL ON)

if(WIN32)
    set(CPACK_GENERATOR WIX)
    set(CPACK_PACKAGE_FILE_NAME
        "PointCloudInspector-${PROJECT_VERSION}-windows-x64")
    set(CPACK_PACKAGE_EXECUTABLES
        "pcinspector" "Point Cloud Inspector")
    set(CPACK_WIX_VERSION 4)
    set(CPACK_WIX_ARCHITECTURE x64)
    set(CPACK_WIX_INSTALL_SCOPE perMachine)
    # Stable UpgradeCode derived from the agreed application identity. It must
    # remain unchanged so newer MSI packages can upgrade earlier versions.
    set(CPACK_WIX_UPGRADE_GUID
        "9BA370D7-AFD1-5ECE-953F-FC9F21BA9590")
    set(CPACK_WIX_PRODUCT_ICON
        "${PROJECT_SOURCE_DIR}/assets/icons/platform/windows/PointCloudInspector.ico")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Point Cloud Inspector")
    set(CPACK_WIX_PROPERTY_ARPCOMMENTS
        "View and inspect LAS, LAZ, COPC, and EPT point clouds")
    set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT
        "https://github.com/dwastberg/pointcloudinspector")
elseif(APPLE)
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_PACKAGE_FILE_NAME
        "PointCloudInspector-${PROJECT_VERSION}-macOS-arm64")
endif()

include(CPack)
