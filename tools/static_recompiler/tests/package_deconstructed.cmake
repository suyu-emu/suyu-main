# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

foreach(required
        POWERSHELL
        PACKAGE_SCRIPT
        PACKAGE_SCHEMA
        RECOMP_TOOL
        NSO_TEST_HELPER
        RECOMP_PACKAGE_TEST_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} was not provided")
    endif()
endforeach()

if(NOT RECOMP_PACKAGE_TEST_DIR MATCHES "[/\\\\]package_deconstructed$")
    message(FATAL_ERROR
        "Refusing to clean an unexpected test directory: ${RECOMP_PACKAGE_TEST_DIR}")
endif()
file(REMOVE_RECURSE "${RECOMP_PACKAGE_TEST_DIR}")
file(MAKE_DIRECTORY
    "${RECOMP_PACKAGE_TEST_DIR}/input/exefs"
    "${RECOMP_PACKAGE_TEST_DIR}/input/romfs/ui")

set(input_dir "${RECOMP_PACKAGE_TEST_DIR}/input")
set(work_dir "${RECOMP_PACKAGE_TEST_DIR}/work")
set(package_dir "${RECOMP_PACKAGE_TEST_DIR}/package")
set(fake_host "${RECOMP_PACKAGE_TEST_DIR}/synthetic-host.exe")

execute_process(
    COMMAND "${NSO_TEST_HELPER}" --write-emittable-fixture "${input_dir}/exefs/main"
    RESULT_VARIABLE nso_result
    OUTPUT_VARIABLE nso_stdout
    ERROR_VARIABLE nso_stderr
)
if(NOT nso_result EQUAL 0)
    message(FATAL_ERROR
        "Synthetic NSO writer failed (${nso_result})\n${nso_stdout}\n${nso_stderr}")
endif()
execute_process(
    COMMAND "${NSO_TEST_HELPER}" --write-npdm "${input_dir}/exefs/main.npdm"
    RESULT_VARIABLE npdm_result
    OUTPUT_VARIABLE npdm_stdout
    ERROR_VARIABLE npdm_stderr
)
if(NOT npdm_result EQUAL 0)
    message(FATAL_ERROR
        "Synthetic NPDM writer failed (${npdm_result})\n${npdm_stdout}\n${npdm_stderr}")
endif()

file(WRITE "${input_dir}/romfs/root.txt" "synthetic RomFS root\n")
file(WRITE "${input_dir}/romfs/ui/message.txt" "synthetic RomFS child\n")
file(WRITE "${input_dir}/romfs/ui/SHA256SUMS" "guest file with checksum basename\n")
file(WRITE "${fake_host}" "synthetic static host fixture - never executed\n")

execute_process(
    COMMAND "${POWERSHELL}"
        -NoProfile
        -NonInteractive
        -ExecutionPolicy Bypass
        -File "${PACKAGE_SCRIPT}"
        -InputRoot "${input_dir}"
        -WorkRoot "${work_dir}"
        -OutputRoot "${package_dir}"
        -RecompilerPath "${RECOMP_TOOL}"
        -StaticHostPath "${fake_host}"
        -LaunchMode Applet
        -AppletId 3
        -AppletType SystemApplet
        -LaunchType FrontendInitiated
        -ProgramIndex 0
        -PreviousProgramIndex -1
    RESULT_VARIABLE package_result
    OUTPUT_VARIABLE package_stdout
    ERROR_VARIABLE package_stderr
)
if(NOT package_result EQUAL 0)
    message(FATAL_ERROR
        "Deconstructed package creation failed (${package_result})\n"
        "${package_stdout}\n${package_stderr}")
endif()

set(expected_package_files
    "LOCAL_ONLY.txt"
    "SHA256SUMS"
    "exefs/main"
    "exefs/main.npdm"
    "launch.cmd"
    "recomp_package.json"
    "romfs/root.txt"
    "romfs/ui/SHA256SUMS"
    "romfs/ui/message.txt"
    "suyu-recompiled.exe")
file(GLOB_RECURSE actual_package_files
    RELATIVE "${package_dir}"
    "${package_dir}/*")
list(SORT actual_package_files)
list(SORT expected_package_files)
if(NOT actual_package_files STREQUAL expected_package_files)
    message(FATAL_ERROR
        "Unexpected package layout\nExpected: ${expected_package_files}\n"
        "Actual:   ${actual_package_files}")
endif()

foreach(expected_aot_file
        "${work_dir}/aot/main/CMakeLists.txt"
        "${work_dir}/aot/main/recomp_export.c"
        "${work_dir}/aot/main/data/text.bin"
        "${work_dir}/aot/recomp_registration.c")
    if(NOT EXISTS "${expected_aot_file}")
        message(FATAL_ERROR "Generated AOT workspace is missing ${expected_aot_file}")
    endif()
endforeach()

file(READ "${PACKAGE_SCHEMA}" schema_json)
string(JSON schema_type ERROR_VARIABLE schema_error TYPE "${schema_json}")
if(schema_error OR NOT schema_type STREQUAL "OBJECT")
    message(FATAL_ERROR "Package schema is not valid JSON: ${schema_error}")
endif()
string(JSON schema_id ERROR_VARIABLE schema_id_error GET "${schema_json}" "$id")
if(schema_id_error OR NOT schema_id STREQUAL "urn:suyu:recomp-package:1")
    message(FATAL_ERROR "Package schema has an unexpected id: ${schema_id_error}")
endif()

file(READ "${package_dir}/recomp_package.json" manifest_json)
foreach(pair
        "format;suyu-deconstructed-recomp-package"
        "format_version;1"
        "local_only;ON"
        "static_module_abi;2"
        "program.program_id;0100000000010000"
        "program.exefs_path;exefs"
        "program.romfs.layout;directory"
        "program.romfs.path;romfs"
        "launch.mode;applet"
        "launch.applet_id;3"
        "launch.applet_type;3"
        "launch.launch_type;0"
        "launch.program_index;0"
        "launch.previous_program_index;-1"
        "artifacts.executable;suyu-recompiled.exe"
        "artifacts.checksums;SHA256SUMS")
    list(GET pair 0 dotted_path)
    list(GET pair 1 expected_value)
    string(REPLACE "." ";" json_path "${dotted_path}")
    string(JSON actual_value ERROR_VARIABLE json_error GET "${manifest_json}" ${json_path})
    if(json_error OR NOT "${actual_value}" STREQUAL "${expected_value}")
        message(FATAL_ERROR
            "Manifest ${dotted_path} mismatch: expected '${expected_value}', "
            "got '${actual_value}' (${json_error})")
    endif()
endforeach()

string(JSON module_count ERROR_VARIABLE module_count_error LENGTH "${manifest_json}" modules)
if(module_count_error OR NOT module_count EQUAL 1)
    message(FATAL_ERROR "Manifest must describe exactly one synthetic module")
endif()
foreach(pair
        "name;main"
        "build_id;808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
        "decoded_text_size;104"
        "mapped_text_size;4096"
        "mapped_text_sha256;63a2c5b7005ef58bb87f253c7092d13b89060b7d6fe2bc4852c9bbb030b0445d"
        "required_hashes_verified;ON")
    list(GET pair 0 field)
    list(GET pair 1 expected_value)
    string(JSON actual_value ERROR_VARIABLE json_error GET "${manifest_json}" modules 0 "${field}")
    if(json_error OR NOT "${actual_value}" STREQUAL "${expected_value}")
        message(FATAL_ERROR
            "Manifest module ${field} mismatch: expected '${expected_value}', "
            "got '${actual_value}' (${json_error})")
    endif()
endforeach()

file(READ "${work_dir}/aot/recomp_registration.c" registration_source)
foreach(expected_registration_text
        "recomp_image_lookup_main"
        "recomp_image_build_id_main"
        "recomp_image_text_sha256_main"
        "recomp_image_text_size_main"
        "suyu_recomp_static_modules_v2")
    string(FIND "${registration_source}" "${expected_registration_text}" registration_position)
    if(registration_position EQUAL -1)
        message(FATAL_ERROR
            "Generated registration is missing '${expected_registration_text}'")
    endif()
endforeach()

file(READ "${package_dir}/launch.cmd" launch_script)
string(FIND "${launch_script}"
    "--game \"%~dp0exefs\\main\"" absolute_game_path_position)
if(absolute_game_path_position EQUAL -1)
    message(FATAL_ERROR "launch.cmd does not use an absolute package-local game path")
endif()
string(FIND "${launch_script}"
    "--applet-params=\"72057594037993472,3,3,0,0,-1\"" applet_parameters_position)
if(applet_parameters_position EQUAL -1)
    message(FATAL_ERROR "launch.cmd does not contain the exact synthetic applet parameters")
endif()

file(READ "${package_dir}/SHA256SUMS" checksum_contents)
foreach(relative_file
        "exefs/main"
        "exefs/main.npdm"
        "romfs/ui/SHA256SUMS"
        "romfs/ui/message.txt"
        "suyu-recompiled.exe")
    file(SHA256 "${package_dir}/${relative_file}" expected_hash)
    string(TOLOWER "${expected_hash}" expected_hash)
    string(FIND "${checksum_contents}" "${expected_hash}  ${relative_file}" hash_position)
    if(hash_position EQUAL -1)
        message(FATAL_ERROR "SHA256SUMS has no correct entry for ${relative_file}")
    endif()
endforeach()

set(previous_checksum_position -1)
foreach(relative_file
        "LOCAL_ONLY.txt"
        "exefs/main"
        "exefs/main.npdm"
        "launch.cmd"
        "recomp_package.json"
        "romfs/root.txt"
        "romfs/ui/SHA256SUMS"
        "romfs/ui/message.txt"
        "suyu-recompiled.exe")
    string(FIND "${checksum_contents}" "  ${relative_file}" checksum_position)
    if(checksum_position LESS 0 OR checksum_position LESS_EQUAL previous_checksum_position)
        message(FATAL_ERROR "SHA256SUMS paths are missing or not sorted ordinally")
    endif()
    set(previous_checksum_position ${checksum_position})
endforeach()

foreach(file_without_private_paths
        "${package_dir}/recomp_package.json"
        "${package_dir}/launch.cmd"
        "${package_dir}/SHA256SUMS")
    file(READ "${file_without_private_paths}" contents)
    string(FIND "${contents}" "${input_dir}" private_path_position)
    if(NOT private_path_position EQUAL -1)
        message(FATAL_ERROR "${file_without_private_paths} leaks the absolute input path")
    endif()
endforeach()

file(WRITE "${package_dir}/sentinel.txt" "preserve me\n")
execute_process(
    COMMAND "${POWERSHELL}"
        -NoProfile
        -NonInteractive
        -ExecutionPolicy Bypass
        -File "${PACKAGE_SCRIPT}"
        -InputRoot "${input_dir}"
        -WorkRoot "${RECOMP_PACKAGE_TEST_DIR}/second-work"
        -OutputRoot "${package_dir}"
        -RecompilerPath "${RECOMP_TOOL}"
        -StaticHostPath "${fake_host}"
    RESULT_VARIABLE repeat_result
    OUTPUT_VARIABLE repeat_stdout
    ERROR_VARIABLE repeat_stderr
)
if(repeat_result EQUAL 0 OR
   NOT "${repeat_stdout}${repeat_stderr}" MATCHES "OutputRoot must be a new path")
    message(FATAL_ERROR
        "Packager did not reject an existing output directory\n"
        "${repeat_stdout}\n${repeat_stderr}")
endif()
file(READ "${package_dir}/sentinel.txt" sentinel_contents)
if(NOT sentinel_contents STREQUAL "preserve me\n")
    message(FATAL_ERROR "Existing output content changed after a rejected package attempt")
endif()

# A packed RomFS can also use the loader's bare sibling `romfs` spelling. The package
# canonicalizes it to exefs/romfs.bin so its launch layout and manifest stay unambiguous.
set(packed_input_dir "${RECOMP_PACKAGE_TEST_DIR}/packed-input")
set(packed_work_dir "${RECOMP_PACKAGE_TEST_DIR}/packed-work")
set(packed_package_dir "${RECOMP_PACKAGE_TEST_DIR}/packed-package")
file(MAKE_DIRECTORY "${packed_input_dir}/exefs")
file(COPY
    "${input_dir}/exefs/main"
    "${input_dir}/exefs/main.npdm"
    DESTINATION "${packed_input_dir}/exefs")
file(WRITE "${packed_input_dir}/romfs" "synthetic packed RomFS\n")

execute_process(
    COMMAND "${POWERSHELL}"
        -NoProfile
        -NonInteractive
        -ExecutionPolicy Bypass
        -File "${PACKAGE_SCRIPT}"
        -InputRoot "${packed_input_dir}"
        -WorkRoot "${packed_work_dir}"
        -OutputRoot "${packed_package_dir}"
        -RecompilerPath "${RECOMP_TOOL}"
        -StaticHostPath "${fake_host}"
    RESULT_VARIABLE packed_result
    OUTPUT_VARIABLE packed_stdout
    ERROR_VARIABLE packed_stderr
)
if(NOT packed_result EQUAL 0)
    message(FATAL_ERROR
        "Bare packed RomFS package creation failed (${packed_result})\n"
        "${packed_stdout}\n${packed_stderr}")
endif()
file(READ "${packed_package_dir}/exefs/romfs.bin" packed_romfs_contents)
if(NOT packed_romfs_contents STREQUAL "synthetic packed RomFS\n")
    message(FATAL_ERROR "Bare sibling RomFS was not copied to exefs/romfs.bin")
endif()
file(READ "${packed_package_dir}/recomp_package.json" packed_manifest_json)
foreach(pair "layout;packed" "path;exefs/romfs.bin")
    list(GET pair 0 field)
    list(GET pair 1 expected_value)
    string(JSON actual_value ERROR_VARIABLE json_error
        GET "${packed_manifest_json}" program romfs "${field}")
    if(json_error OR NOT "${actual_value}" STREQUAL "${expected_value}")
        message(FATAL_ERROR
            "Packed manifest RomFS ${field} mismatch: expected '${expected_value}', "
            "got '${actual_value}' (${json_error})")
    endif()
endforeach()

message(STATUS "Synthetic deconstructed package test passed without executing its host")
