# SPDX-License-Identifier: GPL-3.0-or-later
option(SUYU_EXPORT_BUILD_KIT "Build relocatable Windows AOT host link inputs" OFF)
set(SUYU_EXPORT_BUILD_KIT_REVISION "suyu-aot-kit-abi6-fm1-gg1-fpx1-control-r3")

# Call in src/suyu_cmd before its shared target-configuration loop.
function(suyu_export_build_kit_add_probes)
    if(NOT SUYU_EXPORT_BUILD_KIT)
        return()
    endif()
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
        message(FATAL_ERROR "Export build kit requires Windows MSVC and single-config Ninja")
    endif()
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "Export build kit requires Release")
    endif()
    if(USE_STATIC_MSVC_RUNTIME)
        message(FATAL_ERROR "Export build kit requires the DLL MSVC runtime (/MD)")
    endif()
    enable_language(C)
    foreach(mode strict hybrid)
        set(target "suyu-export-host-${mode}")
        add_executable(${target} ${SUYU_CMD_SOURCES}
            "${PROJECT_SOURCE_DIR}/tools/export_build_kit/registry_probe.c")
        target_compile_definitions(${target} PRIVATE SUYU_CMD_STATIC_RECOMP=1
            SUYU_RECOMP_GUARD_V2=1 SUYU_RECOMP_FASTMEM_V1=1
            SUYU_RECOMP_FEATURES_V1=1 SUYU_RECOMP_GUARD_GEN_V1=1 SUYU_RECOMP_FPX_V1=1)
        if(mode STREQUAL "strict")
            target_compile_definitions(${target} PRIVATE SUYU_CMD_STATIC_RECOMP_STRICT=1)
        else()
            target_compile_definitions(${target} PRIVATE SUYU_CMD_STATIC_RECOMP_HYBRID=1)
        endif()
        list(APPEND SUYU_CMD_TARGETS ${target})
    endforeach()
    set(SUYU_CMD_TARGETS "${SUYU_CMD_TARGETS}" PARENT_SCOPE)
endfunction()

# Call after that loop. Include the resulting export-build-kit directory in ZIPs.
function(suyu_export_build_kit_add_package)
    if(NOT SUYU_EXPORT_BUILD_KIT)
        return()
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(export-build-kit
        COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/export_build_kit/package.py"
            --build "${PROJECT_BINARY_DIR}"
            --output "${PROJECT_BINARY_DIR}/bin/export-build-kit"
            --revision "${SUYU_EXPORT_BUILD_KIT_REVISION}"
        DEPENDS suyu-export-host-strict suyu-export-host-hybrid VERBATIM)
endfunction()
