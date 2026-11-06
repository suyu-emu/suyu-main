# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(suyu_recompiler_tool LANGUAGES C CXX)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "suyu-recomp requires a 64-bit host toolchain")
endif()

add_library(suyu_recompiler_nso STATIC
    ../../src/core/recompiler/nso_dynamic.cpp
    ../../src/core/recompiler/nso_dynamic.h
    ../../src/core/recompiler/nso_image.cpp
    ../../src/core/recompiler/nso_image.h
    ../../src/core/recompiler/nso_relocation_transaction.cpp
    ../../src/core/recompiler/nso_relocation_transaction.h
    ../../src/core/recompiler/nso_relocations.cpp
    ../../src/core/recompiler/nso_relocations.h
    ../../src/core/recompiler/nso_symbols.cpp
    ../../src/core/recompiler/nso_symbols.h
    ../../src/core/recompiler/npdm_info.cpp
    ../../src/core/recompiler/npdm_info.h
)
target_compile_features(suyu_recompiler_nso PUBLIC cxx_std_20)
target_include_directories(suyu_recompiler_nso PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../src")

set(SUYU_RECOMPILER_MBEDTLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../externals/mbedtls")
if(NOT EXISTS "${SUYU_RECOMPILER_MBEDTLS_DIR}/library/sha256.c")
    message(FATAL_ERROR
        "The mbedTLS submodule is required for NSO hash verification; initialize submodules")
endif()
add_library(suyu_recompiler_sha256 STATIC
    nso_sha256.cpp
    nso_sha256.h
    suyu_recompiler_mbedtls_config.h
    "${SUYU_RECOMPILER_MBEDTLS_DIR}/library/platform_util.c"
    "${SUYU_RECOMPILER_MBEDTLS_DIR}/library/sha256.c"
)
target_compile_features(suyu_recompiler_sha256 PRIVATE cxx_std_20)
target_include_directories(suyu_recompiler_sha256 PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(suyu_recompiler_sha256 SYSTEM
    PRIVATE "${SUYU_RECOMPILER_MBEDTLS_DIR}/include")
target_compile_definitions(suyu_recompiler_sha256
    PRIVATE MBEDTLS_CONFIG_FILE="suyu_recompiler_mbedtls_config.h")

add_executable(suyu_recomp suyu_recomp.cpp)
target_compile_features(suyu_recomp PRIVATE cxx_std_20)
target_include_directories(suyu_recomp PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
target_link_libraries(suyu_recomp PRIVATE suyu_recompiler_nso suyu_recompiler_sha256)

if(NOT TARGET lz4::lz4)
    find_package(lz4 CONFIG QUIET)
endif()
if(TARGET lz4::lz4)
    target_link_libraries(suyu_recomp PRIVATE lz4::lz4)
    target_compile_definitions(suyu_recomp PRIVATE SUYU_RECOMPILER_HAS_LZ4=1)
    if(WIN32)
        get_target_property(SUYU_RECOMPILER_LZ4_TYPE lz4::lz4 TYPE)
        if(SUYU_RECOMPILER_LZ4_TYPE STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET suyu_recomp POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:lz4::lz4>" "$<TARGET_FILE_DIR:suyu_recomp>"
                VERBATIM
            )
        endif()
    endif()
endif()
if(WIN32)
    target_link_libraries(suyu_recomp PRIVATE shell32)
endif()

if(MSVC)
    target_compile_options(suyu_recomp PRIVATE /W4 /permissive-)
else()
    target_compile_options(suyu_recomp PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if((BUILD_TESTING OR YUZU_TESTS) AND NOT CMAKE_CROSSCOMPILING)
    add_executable(arm64_to_c_tests tests/arm64_to_c_tests.cpp)
    target_compile_features(arm64_to_c_tests PRIVATE cxx_std_20)
    target_include_directories(arm64_to_c_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
    if(MSVC)
        target_compile_options(arm64_to_c_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(arm64_to_c_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME static_recompiler_arm64_to_c COMMAND arm64_to_c_tests)

    add_executable(nso_dynamic_tests tests/nso_dynamic_tests.cpp)
    target_compile_features(nso_dynamic_tests PRIVATE cxx_std_20)
    target_link_libraries(nso_dynamic_tests PRIVATE suyu_recompiler_nso)
    if(MSVC)
        target_compile_options(nso_dynamic_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(nso_dynamic_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME static_recompiler_nso_dynamic COMMAND nso_dynamic_tests)

    add_executable(nso_symbols_tests tests/nso_symbols_tests.cpp)
    target_compile_features(nso_symbols_tests PRIVATE cxx_std_20)
    target_link_libraries(nso_symbols_tests PRIVATE suyu_recompiler_nso)
    if(MSVC)
        target_compile_options(nso_symbols_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(nso_symbols_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME static_recompiler_nso_symbols COMMAND nso_symbols_tests)

    add_executable(nso_relocations_tests tests/nso_relocations_tests.cpp)
    target_compile_features(nso_relocations_tests PRIVATE cxx_std_20)
    target_link_libraries(nso_relocations_tests PRIVATE suyu_recompiler_nso)
    if(MSVC)
        target_compile_options(nso_relocations_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(nso_relocations_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME static_recompiler_nso_relocations COMMAND nso_relocations_tests)

    add_executable(nso_relocation_transaction_tests tests/nso_relocation_transaction_tests.cpp)
    target_compile_features(nso_relocation_transaction_tests PRIVATE cxx_std_20)
    target_link_libraries(nso_relocation_transaction_tests PRIVATE suyu_recompiler_nso)
    if(MSVC)
        target_compile_options(nso_relocation_transaction_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(nso_relocation_transaction_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME static_recompiler_nso_relocation_transaction
             COMMAND nso_relocation_transaction_tests)

    add_executable(nso_image_tests tests/nso_image_tests.cpp)
    target_compile_features(nso_image_tests PRIVATE cxx_std_20)
    target_link_libraries(nso_image_tests PRIVATE suyu_recompiler_nso suyu_recompiler_sha256)
    if(MSVC)
        target_compile_options(nso_image_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(nso_image_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME static_recompiler_nso_image COMMAND nso_image_tests)
    add_test(
        NAME static_recompiler_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DRECOMP_TOOL=$<TARGET_FILE:suyu_recomp>"
            "-DRECOMP_FIXTURE=${CMAKE_CURRENT_LIST_DIR}/test.bin"
            "-DRECOMP_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke"
            "-DRECOMP_GENERATOR=${CMAKE_GENERATOR}"
            "-DRECOMP_C_COMPILER=${CMAKE_C_COMPILER}"
            "-P" "${CMAKE_CURRENT_LIST_DIR}/tests/smoke.cmake"
    )
    set_tests_properties(static_recompiler_smoke PROPERTIES TIMEOUT 120)

    add_test(
        NAME static_recompiler_nso_inspect
        COMMAND "${CMAKE_COMMAND}"
            "-DRECOMP_TOOL=$<TARGET_FILE:suyu_recomp>"
            "-DNSO_TEST_HELPER=$<TARGET_FILE:nso_image_tests>"
            "-DNSO_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/nso_inspect"
            "-P" "${CMAKE_CURRENT_LIST_DIR}/tests/nso_inspect.cmake"
    )

    add_test(
        NAME static_recompiler_nso_emit
        COMMAND "${CMAKE_COMMAND}"
            "-DRECOMP_TOOL=$<TARGET_FILE:suyu_recomp>"
            "-DNSO_TEST_HELPER=$<TARGET_FILE:nso_image_tests>"
            "-DNSO_EMIT_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/nso_emit"
            "-DRECOMP_GENERATOR=${CMAKE_GENERATOR}"
            "-DRECOMP_C_COMPILER=${CMAKE_C_COMPILER}"
            "-P" "${CMAKE_CURRENT_LIST_DIR}/tests/nso_emit.cmake"
    )
    if(WIN32 AND MSVC)
        # MSVC can spend more than two minutes compiling the generated static image even after
        # emission and configuration have succeeded on hosted runners.
        set_tests_properties(static_recompiler_nso_emit PROPERTIES TIMEOUT 300)
    else()
        set_tests_properties(static_recompiler_nso_emit PROPERTIES TIMEOUT 120)
    endif()

    if(WIN32)
        find_program(SUYU_RECOMPILER_POWERSHELL NAMES pwsh powershell)
        if(SUYU_RECOMPILER_POWERSHELL)
            add_test(
                NAME static_recompiler_package_deconstructed
                COMMAND "${CMAKE_COMMAND}"
                    "-DPOWERSHELL=${SUYU_RECOMPILER_POWERSHELL}"
                    "-DPACKAGE_SCRIPT=${CMAKE_CURRENT_LIST_DIR}/package_deconstructed.ps1"
                    "-DPACKAGE_SCHEMA=${CMAKE_CURRENT_LIST_DIR}/recomp_package.schema.json"
                    "-DRECOMP_TOOL=$<TARGET_FILE:suyu_recomp>"
                    "-DNSO_TEST_HELPER=$<TARGET_FILE:nso_image_tests>"
                    "-DRECOMP_PACKAGE_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/package_deconstructed"
                    "-P" "${CMAKE_CURRENT_LIST_DIR}/tests/package_deconstructed.cmake"
            )
            set_tests_properties(static_recompiler_package_deconstructed PROPERTIES TIMEOUT 120)
        else()
            message(STATUS "PowerShell was not found; skipping deconstructed package test")
        endif()
    endif()
endif()
