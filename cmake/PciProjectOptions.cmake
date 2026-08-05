add_library(pcinspector_project_options INTERFACE)
add_library(pcinspector::project_options ALIAS pcinspector_project_options)
target_compile_features(pcinspector_project_options INTERFACE cxx_std_23)
target_compile_definitions(pcinspector_project_options INTERFACE
        $<$<BOOL:${PCINSPECTOR_ENABLE_DIAGNOSTIC_UI}>:PCINSPECTOR_ENABLE_DIAGNOSTIC_UI>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>)

option(PCINSPECTOR_ENABLE_CLANG_TIDY
       "Run the gating project clang-tidy checks while compiling" OFF)
if(PCINSPECTOR_ENABLE_CLANG_TIDY)
    find_program(PCINSPECTOR_CLANG_TIDY_EXECUTABLE
                 NAMES clang-tidy
                 HINTS
                 "$ENV{LLVM_ROOT}/bin"
                 /opt/homebrew/opt/llvm/bin
                 /usr/local/opt/llvm/bin
                 DOC "clang-tidy executable used by the static-analysis preset")
    if(NOT PCINSPECTOR_CLANG_TIDY_EXECUTABLE)
        message(FATAL_ERROR
                "The clang-tidy preset requires clang-tidy; install LLVM or set LLVM_ROOT")
    endif()
    set(PCINSPECTOR_CLANG_TIDY_COMMAND
        "${PCINSPECTOR_CLANG_TIDY_EXECUTABLE};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy")
endif()

set(PCINSPECTOR_SANITIZER "none" CACHE STRING
    "Runtime sanitizer instrumentation: none, address-undefined, or thread")
set_property(CACHE PCINSPECTOR_SANITIZER PROPERTY STRINGS
             none address-undefined thread)
set(pcinspector_supported_sanitizers
    none address-undefined thread)

if(NOT PCINSPECTOR_SANITIZER IN_LIST
        pcinspector_supported_sanitizers)
    message(FATAL_ERROR
            "PCINSPECTOR_SANITIZER must be one of: none, address-undefined, thread")
endif()
unset(pcinspector_supported_sanitizers)

if(NOT PCINSPECTOR_SANITIZER STREQUAL "none")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        message(FATAL_ERROR
                "${PCINSPECTOR_SANITIZER} sanitizer instrumentation requires Clang or GCC")
    endif()

    if(PCINSPECTOR_SANITIZER STREQUAL "address-undefined")
        set(pcinspector_sanitizer_flags
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
    elseif(PCINSPECTOR_SANITIZER STREQUAL "thread")
        set(pcinspector_sanitizer_flags
            -fsanitize=thread
            -fno-omit-frame-pointer)
    endif()

    target_compile_options(pcinspector_project_options INTERFACE
            ${pcinspector_sanitizer_flags})
    target_link_options(pcinspector_project_options INTERFACE
            ${pcinspector_sanitizer_flags})
    unset(pcinspector_sanitizer_flags)
endif()

add_library(pcinspector_project_warnings INTERFACE)
add_library(pcinspector::project_warnings ALIAS pcinspector_project_warnings)

if(MSVC)
    target_compile_options(pcinspector_project_warnings INTERFACE
            /W4
            /permissive-
            /Zc:__cplusplus
            /utf-8)
    if(PCINSPECTOR_WARNINGS_AS_ERRORS)
        target_compile_options(pcinspector_project_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(pcinspector_project_warnings INTERFACE
            -Wall
            -Wextra
            -Wpedantic)
    if(PCINSPECTOR_WARNINGS_AS_ERRORS)
        target_compile_options(pcinspector_project_warnings INTERFACE -Werror)
    endif()
endif()

function(pci_configure_target target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "pci_configure_target: unknown target '${target}'")
    endif()

    target_link_libraries(${target} PRIVATE
            pcinspector::project_options
            pcinspector::project_warnings)
    set_target_properties(${target} PROPERTIES
            CXX_EXTENSIONS OFF)
    if(PCINSPECTOR_ENABLE_CLANG_TIDY)
        set_target_properties(${target} PROPERTIES
                CXX_CLANG_TIDY "${PCINSPECTOR_CLANG_TIDY_COMMAND}")
    endif()
endfunction()
