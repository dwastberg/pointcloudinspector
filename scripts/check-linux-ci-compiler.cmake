# Check CMake's selected compiler, including when an existing build is reused.
include("${BUILD_DIR}/CMakeFiles/${CMAKE_VERSION}/CMakeCXXCompiler.cmake")
message(STATUS "CI compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "CI compiler ID/version: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Linux ci and ci-release require GCC; check the activated micromamba environment.")
endif()
