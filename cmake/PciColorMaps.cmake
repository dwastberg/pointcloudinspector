set(PCINSPECTOR_COLOR_MAP_DIRECTORY
    "${PROJECT_SOURCE_DIR}/assets/colormaps"
    CACHE PATH
    "Directory containing CPT color maps embedded in the application")

function(pci_add_cpt_resources target resource_name)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "DIRECTORY" "")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
                "pci_add_cpt_resources: unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR
                "pci_add_cpt_resources: unknown target '${target}'")
    endif()
    if(ARG_DIRECTORY)
        set(configured_directory "${ARG_DIRECTORY}")
    else()
        set(configured_directory
                "${PCINSPECTOR_COLOR_MAP_DIRECTORY}")
    endif()
    get_filename_component(color_map_directory
            "${configured_directory}"
            ABSOLUTE
            BASE_DIR "${PROJECT_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${color_map_directory}")
        message(FATAL_ERROR
                "PCINSPECTOR_COLOR_MAP_DIRECTORY is not a directory: ${color_map_directory}")
    endif()

    # Auto-discovery is deliberate for this asset directory. CONFIGURE_DEPENDS
    # makes additions and removals regenerate the resource inventory on the
    # next build while ordinary edits remain normal file dependencies.
    file(GLOB_RECURSE color_map_files
            CONFIGURE_DEPENDS
            LIST_DIRECTORIES FALSE
            "${color_map_directory}/*.cpt")
    list(SORT color_map_files)
    if(NOT color_map_files)
        return()
    endif()

    set(case_folded_paths)
    foreach(color_map_file IN LISTS color_map_files)
        file(RELATIVE_PATH relative_path
                "${color_map_directory}"
                "${color_map_file}")
        string(TOLOWER "${relative_path}" case_folded_path)
        if(case_folded_path IN_LIST case_folded_paths)
            message(FATAL_ERROR
                    "CPT paths must be unique when compared case-insensitively: ${relative_path}")
        endif()
        list(APPEND case_folded_paths "${case_folded_path}")
    endforeach()

    qt_add_resources(${target} ${resource_name}
            PREFIX "/colormaps"
            BASE "${color_map_directory}"
            FILES ${color_map_files})
endfunction()
