
CPMAddPackage(
    NAME FastSIMD
    GITHUB_REPOSITORY Auburn/FastSIMD
    GIT_TAG 16450dae9528727e500e7254f635a671f9c7ee2d
)

set(install_targets ${install_targets}
    FastNoise
    FastSIMD
    FastSIMD_FastNoise  
    PARENT_SCOPE)

install(
    DIRECTORY "${FastNoise2_SOURCE_DIR}/include/FastNoise"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h"
)
install(
    DIRECTORY "${FastSIMD_SOURCE_DIR}/include/FastSIMD/Utility"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/FastSIMD"
    FILES_MATCHING PATTERN "*.h"
)
install(
    FILES "${FastSIMD_SOURCE_DIR}/include/FastSIMD/DispatchClass.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/FastSIMD"
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/fastsimd/FastSIMD_FastNoise/include/FastSIMD/FastSIMD_FastNoise_config.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/FastSIMD"
)


file(GLOB FastNoise_headers "../include/FastNoise/*.h")
file(GLOB_RECURSE FastNoise_generators "../include/FastNoise/Generators/*")
file(GLOB_RECURSE FastNoise_utility "../include/FastNoise/Utility/*")
file(GLOB_RECURSE FastNoise_source "../src/*.h" "../src/*.cpp" "../src/*.inl")

source_group("FastNoise" FILES ${FastNoise_headers})
source_group("FastNoise" FILES ${FastNoise_source})
source_group("FastNoise/Generators" FILES ${FastNoise_generators})
source_group("FastNoise/Utility" FILES ${FastNoise_utility})

add_library(FastNoise
    ${FastNoise_headers}
    ${FastNoise_source}
    ${FastNoise_generators}
    ${FastNoise_utility}
)

add_library(FastNoise2 ALIAS FastNoise)

target_include_directories(FastNoise PUBLIC 
    $<BUILD_INTERFACE:${FastNoise2_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
   
target_compile_definitions(FastNoise PRIVATE FASTNOISE_EXPORT)
target_compile_definitions(FastNoise PUBLIC FASTNOISE2_VERSION="${FastNoise2_VERSION}")

set_target_properties(FastNoise PROPERTIES
    DEBUG_POSTFIX D
    COMPILE_PDB_NAME_DEBUG FastNoiseD)

if(NOT FASTNOISE2_STRICT_FP)
    set(FASTSIMD_RELAXED RELAXED)
endif()

fastsimd_create_dispatch_library(FastSIMD_FastNoise ${FASTSIMD_RELAXED} SOURCES "FastNoise/FastSIMD_Build.inl")

target_include_directories(FastSIMD_FastNoise PRIVATE "../include/")

option(FASTNOISE2_STATIC_EXPORT_API "Build a single static archive, compiled with export annotations, for a shared library that re-exports the FastNoise2 API" OFF)

if(FASTNOISE2_STATIC_EXPORT_API AND NOT BUILD_SHARED_LIBS)
    # The archive is linked whole into a host shared library that re-exports the
    # FastNoise2 and FastSIMD API, so every module in the process shares ONE
    # metadata registry and ONE feature-set detection. Two things differ from a
    # plain static build:
    #
    # - Export annotations stay on. FastSIMD marks itself FASTSIMD_STATIC_LIB,
    #   which empties FASTSIMD_API and, through Utility/Export.h, FASTNOISE_API
    #   too; it is removed here, and every object compiles with *_EXPORT
    #   (dllexport on Windows, default visibility elsewhere). Consumers compile
    #   with neither macro and import from the host library.
    # - The FastSIMD and dispatch objects are folded into this archive with no
    #   link edge to their targets, so the installed package is one library.
    #   Linking them as targets would also hand consumers -msimd128 (Emscripten),
    #   which only the dispatch objects need.
    foreach(fastsimd_target FastSIMD FastSIMD_FastNoise)
        foreach(definition_property COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
            get_target_property(fastsimd_definitions ${fastsimd_target} ${definition_property})
            if(fastsimd_definitions)
                list(REMOVE_ITEM fastsimd_definitions FASTSIMD_STATIC_LIB)
                set_target_properties(${fastsimd_target} PROPERTIES ${definition_property} "${fastsimd_definitions}")
            endif()
        endforeach()
    endforeach()

    target_compile_definitions(FastNoise PRIVATE FASTSIMD_EXPORT)
    target_sources(FastNoise PRIVATE $<TARGET_OBJECTS:FastSIMD> $<TARGET_OBJECTS:FastSIMD_FastNoise>)
    target_include_directories(FastNoise PUBLIC
        $<BUILD_INTERFACE:${FastSIMD_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/fastsimd/FastSIMD_FastNoise/include>
    )
    add_dependencies(FastNoise FastSIMD FastSIMD_FastNoise)

    set(install_targets FastNoise PARENT_SCOPE)
else()
    if(NOT BUILD_SHARED_LIBS)
        target_compile_definitions(FastNoise PUBLIC FASTNOISE_STATIC_LIB)
    endif()

    target_link_libraries(FastNoise PUBLIC FastSIMD FastSIMD_FastNoise)
endif()

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    target_compile_options(FastSIMD_FastNoise PRIVATE /GL- /GS- /wd4251 /permissive- /d2vzeroupper-)
    
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
    if(MSVC)
        target_compile_options(FastSIMD_FastNoise PRIVATE /GS-)
    else()
        target_compile_options(FastSIMD_FastNoise PRIVATE -fno-stack-protector -Wno-nan-infinity-disabled)
    endif()

    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        target_compile_options(FastSIMD_FastNoise PRIVATE -mno-vzeroupper)
    else()
        target_compile_options(FastSIMD_FastNoise PRIVATE -mllvm -x86-use-vzeroupper=0)        
    endif()
endif()

if(NOT FASTNOISE2_STRICT_FP)
    if(MSVC)
        target_compile_options(FastSIMD_FastNoise PRIVATE /fp:fast)
    else()
        target_compile_options(FastSIMD_FastNoise PRIVATE -ffast-math)   
    endif()
endif()
