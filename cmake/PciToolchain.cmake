include_guard(GLOBAL)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
        message(FATAL_ERROR
                "Point Cloud Inspector requires GCC 12 or newer; found GCC ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(FATAL_ERROR
                "Point Cloud Inspector requires Clang 16 or newer; found Clang ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        message(FATAL_ERROR
                "Point Cloud Inspector requires AppleClang 15 or newer; found AppleClang ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(MSVC)
    if(MSVC_VERSION LESS 1933)
        message(FATAL_ERROR
                "Point Cloud Inspector requires MSVC 19.33 or newer; found MSVC ${MSVC_VERSION}")
    endif()
else()
    message(FATAL_ERROR
            "Point Cloud Inspector supports GCC 12+, Clang 16+, AppleClang 15+, and MSVC 19.33+; found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

include(CheckCXXSourceCompiles)
if(DEFINED CMAKE_CXX_STANDARD)
    set(pcinspector_had_cxx_standard TRUE)
endif()
if(DEFINED CMAKE_CXX_STANDARD_REQUIRED)
    set(pcinspector_had_cxx_standard_required TRUE)
endif()
if(DEFINED CMAKE_CXX_EXTENSIONS)
    set(pcinspector_had_cxx_extensions TRUE)
endif()
set(pcinspector_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
set(pcinspector_saved_cxx_standard_required
    "${CMAKE_CXX_STANDARD_REQUIRED}")
set(pcinspector_saved_cxx_extensions "${CMAKE_CXX_EXTENSIONS}")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
check_cxx_source_compiles(
    "#include <expected>
     #if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
     #error std::expected is unavailable
     #endif
     int main()
     {
         std::expected<int, int> value = 42;
         return value.value() == 42 ? 0 : 1;
    }"
    PCINSPECTOR_HAS_STD_EXPECTED)
if(pcinspector_had_cxx_standard)
    set(CMAKE_CXX_STANDARD "${pcinspector_saved_cxx_standard}")
else()
    unset(CMAKE_CXX_STANDARD)
endif()
if(pcinspector_had_cxx_standard_required)
    set(CMAKE_CXX_STANDARD_REQUIRED
        "${pcinspector_saved_cxx_standard_required}")
else()
    unset(CMAKE_CXX_STANDARD_REQUIRED)
endif()
if(pcinspector_had_cxx_extensions)
    set(CMAKE_CXX_EXTENSIONS "${pcinspector_saved_cxx_extensions}")
else()
    unset(CMAKE_CXX_EXTENSIONS)
endif()
unset(pcinspector_had_cxx_standard)
unset(pcinspector_had_cxx_standard_required)
unset(pcinspector_had_cxx_extensions)
unset(pcinspector_saved_cxx_standard)
unset(pcinspector_saved_cxx_standard_required)
unset(pcinspector_saved_cxx_extensions)

if(NOT PCINSPECTOR_HAS_STD_EXPECTED)
    message(FATAL_ERROR
            "Point Cloud Inspector requires a C++23 standard library with std::expected (__cpp_lib_expected >= 202202L)")
endif()
