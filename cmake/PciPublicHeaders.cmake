# Compile every exported header with only its owning target's public dependencies.
set(pci_public_modules
    foundation color pointcloud raster raster_runtime vector document navigation
    tasking render_telemetry runtime_resources point_runtime scene_runtime
    operation_api operation_local operations frame platform secure_storage
    renderer_planning development_support viewport_api app_config app_model
    desktop_session import_pdal gdal_runtime import_gdal import_ogr
    desktop_dispatch desktop_operations app_ui renderer viewport)
foreach(module IN LISTS pci_public_modules)
    get_target_property(include_roots pcinspector_${module} INTERFACE_INCLUDE_DIRECTORIES)
    set(headers "")
    foreach(include_root IN LISTS include_roots)
        if(include_root MATCHES "/include$")
            file(GLOB_RECURSE exported CONFIGURE_DEPENDS RELATIVE "${include_root}" "${include_root}/*.h")
            list(APPEND headers ${exported})
        endif()
    endforeach()
    if(headers)
        pci_add_header_self_containment(${module} pcinspector::${module} ${headers})
    endif()
endforeach()
pci_add_header_self_containment_target()
