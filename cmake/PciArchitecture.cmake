include_guard(GLOBAL)

function(pci_normalize_link_item output item)
    set(normalized "${item}")
    if(normalized MATCHES "^\\$<LINK_ONLY:(.*)>$")
        set(normalized "${CMAKE_MATCH_1}")
    endif()
    if(TARGET "${normalized}")
        get_target_property(aliased_target "${normalized}" ALIASED_TARGET)
        if(aliased_target)
            set(normalized "${aliased_target}")
        endif()
    endif()
    set(${output} "${normalized}" PARENT_SCOPE)
endfunction()

function(pci_register_architecture_checks)
    set(target_names
        pcinspector_foundation
        pcinspector_operation_api
        pcinspector_operation_local
        pcinspector_platform
        pcinspector_secure_storage
        pcinspector_navigation
        pcinspector_render_telemetry
        pcinspector_color
        pcinspector_pointcloud
        pcinspector_raster
        pcinspector_raster_runtime
        pcinspector_vector
        pcinspector_document
        pcinspector_tasking
        pcinspector_runtime_resources
        pcinspector_point_runtime
        pcinspector_scene_runtime
        pcinspector_operations
        pcinspector_development_support
        pcinspector_viewport_api
        pcinspector_renderer_planning
        pcinspector_frame
        pcinspector_viewport
        pcinspector_desktop_dispatch
        pcinspector_app_config
        pcinspector_app_model
        pcinspector_desktop_session
        pcinspector_import_pdal
        pcinspector_gdal_runtime
        pcinspector_import_gdal
        pcinspector_import_ogr
        pcinspector_desktop_operations
        pcinspector_app_ui
        pcinspector_renderer
        pcinspector)

    set(allowed_pcinspector_foundation "")
    set(allowed_pcinspector_operation_api
        pcinspector_foundation
        pcinspector_pointcloud
        pcinspector_raster
        pcinspector_runtime_resources
        pcinspector_vector)
    set(allowed_pcinspector_operation_local
        pcinspector_operation_api
        pcinspector_platform
        pcinspector_secure_storage
        Qt6::Core)
    set(allowed_pcinspector_platform pcinspector_foundation Qt6::Core Psapi)
    set(allowed_pcinspector_secure_storage pcinspector_platform Qt6::Core Advapi32)
    set(allowed_pcinspector_navigation pcinspector_foundation)
    set(allowed_pcinspector_render_telemetry pcinspector_foundation)
    set(allowed_pcinspector_color "")
    set(allowed_pcinspector_pointcloud
        pcinspector_color pcinspector_foundation)
    set(allowed_pcinspector_raster
        pcinspector_color pcinspector_foundation)
    set(allowed_pcinspector_raster_runtime pcinspector_raster)
    set(allowed_pcinspector_vector
        pcinspector_earcut pcinspector_foundation)
    set(allowed_pcinspector_document
        pcinspector_color
        pcinspector_foundation
        pcinspector_pointcloud
        pcinspector_raster
        pcinspector_vector)
    set(allowed_pcinspector_tasking pcinspector_foundation)
    set(allowed_pcinspector_runtime_resources pcinspector_foundation)
    set(allowed_pcinspector_point_runtime
        pcinspector_foundation
        pcinspector_pointcloud
        pcinspector_runtime_resources
        pcinspector_tasking)
    set(allowed_pcinspector_scene_runtime
        pcinspector_foundation
        pcinspector_pointcloud
        pcinspector_point_runtime
        pcinspector_raster
        pcinspector_runtime_resources
        pcinspector_tasking)
    set(allowed_pcinspector_operations
        pcinspector_operation_api
        pcinspector_point_runtime
        pcinspector_tasking)
    set(allowed_pcinspector_development_support
        pcinspector_point_runtime pcinspector_pointcloud)
    set(allowed_pcinspector_viewport_api
        pcinspector_renderer_planning
        pcinspector_color
        pcinspector_document
        pcinspector_scene_runtime
        Qt6::Core)
    set(allowed_pcinspector_renderer_planning
        pcinspector_document
        pcinspector_foundation
        pcinspector_navigation
        pcinspector_pointcloud
        pcinspector_raster)
    set(allowed_pcinspector_frame
        pcinspector_render_telemetry
        pcinspector_foundation
        pcinspector_document
        pcinspector_raster_runtime
        pcinspector_renderer_planning
        pcinspector_scene_runtime)
    # Both planners consume immutable data contracts. Mutable runtime access
    # belongs exclusively to frame execution.
    set(allowed_pcinspector_app_config
        pcinspector_foundation
        pcinspector_platform
        pcinspector_viewport_api)
    set(allowed_pcinspector_app_model
        pcinspector_operation_api pcinspector_viewport_api)
    set(allowed_pcinspector_desktop_session
        pcinspector_app_config
        pcinspector_app_model
        pcinspector_color
        pcinspector_document
        pcinspector_desktop_operations
        pcinspector_platform
        pcinspector_raster
        pcinspector_scene_runtime
        Qt6::Core)
    set(allowed_pcinspector_import_pdal
        pcinspector_platform
        pcinspector_operation_api
        pcinspector_secure_storage
        PDAL::pdalcpp
        Qt6::Core)
    set(allowed_pcinspector_gdal_runtime GDAL::GDAL)
    set(allowed_pcinspector_import_gdal
        pcinspector_color
        pcinspector_gdal_runtime
        pcinspector_operation_api
        pcinspector_raster)
    set(allowed_pcinspector_import_ogr
        pcinspector_gdal_runtime pcinspector_operation_api pcinspector_vector)
    set(allowed_pcinspector_desktop_dispatch pcinspector_runtime_resources Qt6::Core)
    set(allowed_pcinspector_viewport pcinspector_viewport_api pcinspector_renderer pcinspector_frame pcinspector_render_telemetry pcinspector_desktop_dispatch pcinspector_platform Qt6::Widgets Qt6::GuiPrivate)
    set(allowed_pcinspector_desktop_operations
        pcinspector_desktop_dispatch
        pcinspector_document
        pcinspector_foundation
        pcinspector_operation_api
        pcinspector_operations
        pcinspector_platform
        pcinspector_raster
        pcinspector_runtime_resources
        pcinspector_tasking
        Qt6::Core)
    set(allowed_pcinspector_app_ui
        pcinspector_app_config
        pcinspector_app_model
        pcinspector_desktop_session
        pcinspector_color
        pcinspector_document
        pcinspector_desktop_operations
        pcinspector_pointcloud
        pcinspector_viewport_api
        pcinspector_vector
        Qt6::Widgets
        pcinspector_platform)
    set(allowed_pcinspector_renderer
        pcinspector_color
        pcinspector_document
        pcinspector_navigation
        pcinspector_platform
        pcinspector_pointcloud
        pcinspector_raster
        pcinspector_raster_runtime
        pcinspector_render_telemetry
        pcinspector_frame
        pcinspector_renderer_planning
        pcinspector_scene_runtime
        pcinspector_vector
        Qt6::GuiPrivate
        Qt6::Gui)
    set(allowed_pcinspector
        pcinspector_app_config
        pcinspector_app_ui
        pcinspector_color
        pcinspector_development_support
        pcinspector_desktop_operations
        pcinspector_import_gdal
        pcinspector_import_ogr
        pcinspector_import_pdal
        pcinspector_operation_local
        pcinspector_platform
        pcinspector_viewport
        pcinspector_secure_storage
        Qt6::Core)

    if(PCINSPECTOR_BUILD_TOOLS)
        list(APPEND target_names pci_load_bench pci_residency_bench pci_multifile_bench
            pci_raster_colorize_bench pci_qualification_diff)
        set(allowed_pci_load_bench pcinspector_app_config pcinspector_import_pdal pcinspector_operations pcinspector_platform)
        set(allowed_pci_residency_bench pcinspector_app_config pcinspector_document pcinspector_import_pdal pcinspector_operations pcinspector_platform pcinspector_scene_runtime)
        set(allowed_pci_multifile_bench pcinspector_app_config pcinspector_document pcinspector_desktop_operations pcinspector_import_pdal pcinspector_operations pcinspector_platform pcinspector_scene_runtime)
        set(allowed_pci_raster_colorize_bench pcinspector_operation_local pcinspector_operations)
        set(allowed_pci_qualification_diff pcinspector_platform Qt6::Core)
    endif()

    set(manifest "# Generated by pci_register_architecture_checks.\n")
    string(APPEND manifest "set(PCI_ARCHITECTURE_TARGETS ${target_names})\n")
    foreach(target IN LISTS target_names)
        get_target_property(public_includes "${target}" INTERFACE_INCLUDE_DIRECTORIES)
        get_target_property(private_includes "${target}" INCLUDE_DIRECTORIES)
        foreach(include_root IN LISTS public_includes private_includes)
            if(include_root STREQUAL "${PROJECT_SOURCE_DIR}/src")
                message(FATAL_ERROR "${target} exposes the legacy global source include root")
            endif()
        endforeach()

        if(NOT TARGET "${target}")
            message(FATAL_ERROR "Architecture target '${target}' does not exist")
        endif()
        get_target_property(target_type "${target}" TYPE)
        if(target_type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(link_items "${target}" INTERFACE_LINK_LIBRARIES)
        else()
            get_target_property(link_items "${target}" LINK_LIBRARIES)
        endif()
        if(NOT link_items)
            set(link_items "")
        endif()

        set(observed "")
        foreach(item IN LISTS link_items)
            # qt_add_executable appends platform-plugin selection expressions.
            # They are Qt implementation detail, not project-authored edges.
            if(item MATCHES "^\\$<" AND item MATCHES "QT_IS_PLUGIN_GENEX")
                continue()
            endif()
            pci_normalize_link_item(normalized "${item}")
            if(normalized MATCHES
                    "^pcinspector_(project_options|project_warnings)$")
                continue()
            endif()
            list(APPEND observed "${normalized}")
        endforeach()
        list(REMOVE_DUPLICATES observed)
        list(SORT observed)
        set(expected "")
        foreach(item IN LISTS allowed_${target})
            pci_normalize_link_item(normalized "${item}")
            list(APPEND expected "${normalized}")
        endforeach()
        if(APPLE AND target STREQUAL "pcinspector_platform")
            list(REMOVE_ITEM expected Psapi)
        elseif(NOT WIN32 AND target STREQUAL "pcinspector_platform")
            list(REMOVE_ITEM expected Psapi)
        endif()
        if(NOT WIN32 AND target STREQUAL "pcinspector_secure_storage")
            list(REMOVE_ITEM expected Advapi32)
        endif()
        list(REMOVE_DUPLICATES expected)
        list(SORT expected)
        string(APPEND manifest
               "set(PCI_OBSERVED_${target} [==[${observed}]==])\n")
        string(APPEND manifest
               "set(PCI_EXPECTED_${target} [==[${expected}]==])\n")
    endforeach()

    set(manifest_path
        "${CMAKE_CURRENT_BINARY_DIR}/PciArchitectureTargets.cmake")
    file(WRITE "${manifest_path}" "${manifest}")

    add_test(NAME architecture_include_boundaries
        COMMAND "${CMAKE_COMMAND}"
                "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                -P "${PROJECT_SOURCE_DIR}/cmake/CheckIncludeBoundaries.cmake")
    add_test(NAME architecture_target_boundaries
        COMMAND "${CMAKE_COMMAND}"
                "-DMANIFEST=${manifest_path}"
                -P "${PROJECT_SOURCE_DIR}/cmake/CheckTargetBoundaries.cmake")
    set_tests_properties(
        architecture_include_boundaries architecture_target_boundaries
        PROPERTIES LABELS "architecture")
endfunction()
