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
    include("${CMAKE_CURRENT_LIST_DIR}/PciPackaging.cmake")
endif()
