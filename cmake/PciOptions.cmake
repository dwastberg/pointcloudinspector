include(CMakeDependentOption)

option(PCINSPECTOR_BUILD_TOOLS
       "Build developer profiling and diagnostic tools" OFF)
option(PCINSPECTOR_BATCH_TESTS
       "Run ordinary Catch2 suites in one process per executable" OFF)
option(PCINSPECTOR_ENABLE_LONG_STRESS_TESTS
       "Register expensive full-scale stress tests" OFF)
option(PCINSPECTOR_CHECK_HEADERS
       "Include public header self-containment checks in the default build" ON)
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
