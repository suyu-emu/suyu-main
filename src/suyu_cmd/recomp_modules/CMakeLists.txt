# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds the generated modules of one static export as a project of their own.
#
# CMake allows one C compiler per project, and the suyu tree uses MSVC. The
# Windows "Build" export configures this project with
# CMAKE_C_COMPILER=clang-cl, builds one recomp_static_<module> target per
# module, and then links the libraries in lib/ into suyu-cmd-static with MSVC's
# link.exe (see SUYU_CMD_RECOMP_PREBUILT_DIR in ../CMakeLists.txt). On MK8D the
# same generated C races ~45% faster built with clang-cl than with MSVC.
#
# Inputs:
#   SUYU_CMD_RECOMP_DIR  the export's exefs/ directory (as for suyu_cmd)
#   SUYU_RECOMP_MODULES  the module subdirectories to build, e.g. main;sdk
cmake_minimum_required(VERSION 3.22)
project(suyu_recomp_modules C)

if (NOT SUYU_CMD_RECOMP_DIR OR NOT EXISTS "${SUYU_CMD_RECOMP_DIR}/recomp_registration.c")
    message(FATAL_ERROR "SUYU_CMD_RECOMP_DIR must point at an export's exefs/ directory")
endif()
if (NOT SUYU_RECOMP_MODULES)
    message(FATAL_ERROR "SUYU_RECOMP_MODULES is empty")
endif()

# Every library in one place, so the suyu tree can import them by name.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# The definitions suyu's own tree applies to these units on Windows.
add_compile_definitions(BUILD_ID="amd64" _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
    _SCL_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if (MSVC)
    # Code generation switches suyu uses, minus /QIntel-jcc-erratum and /GL.
    # The generated CMakeLists add /O2 (and /bigobj, which clang-cl accepts).
    add_compile_options(/utf-8 /GS- /Gy)
endif()
if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Machine-written code; nobody reads its warnings, and printing them costs time.
    add_compile_options(-Wno-everything)
endif()

# Tells the generated module projects to emit only the static library.
set(RECOMP_STATIC_ONLY ON)
foreach(_m ${SUYU_RECOMP_MODULES})
    if (NOT EXISTS "${SUYU_CMD_RECOMP_DIR}/${_m}/CMakeLists.txt")
        message(FATAL_ERROR "No generated module '${_m}' in ${SUYU_CMD_RECOMP_DIR}")
    endif()
    add_subdirectory("${SUYU_CMD_RECOMP_DIR}/${_m}" "${CMAKE_CURRENT_BINARY_DIR}/recomp/${_m}")
    # Same renames as ../CMakeLists.txt applies (see the comment there).
    target_compile_definitions(recomp_static_${_m} PRIVATE
        recomp_image_index=recomp_image_index_${_m}
        _recomp_index_view=_recomp_index_view_${_m}
        recomp_build_index=recomp_build_index_${_m})
endforeach()
