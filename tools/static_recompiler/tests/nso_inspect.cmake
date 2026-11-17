# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED RECOMP_TOOL OR NOT DEFINED NSO_TEST_HELPER OR NOT DEFINED NSO_TEST_DIR)
    message(FATAL_ERROR "NSO inspect test is missing a required path")
endif()

if(NOT NSO_TEST_DIR MATCHES "[/\\\\]nso_inspect$")
    message(FATAL_ERROR "Refusing to clean an unexpected test directory: ${NSO_TEST_DIR}")
endif()

file(REMOVE_RECURSE "${NSO_TEST_DIR}")
file(MAKE_DIRECTORY "${NSO_TEST_DIR}")
set(NSO_FIXTURE "${NSO_TEST_DIR}/synthetic.nso")
set(NPDM_FIXTURE "${NSO_TEST_DIR}/main.npdm")

execute_process(
    COMMAND "${NSO_TEST_HELPER}" --write-fixture "${NSO_FIXTURE}"
    RESULT_VARIABLE FIXTURE_RESULT
    OUTPUT_VARIABLE FIXTURE_OUTPUT
    ERROR_VARIABLE FIXTURE_ERROR
)
if(NOT FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not create synthetic NSO: ${FIXTURE_OUTPUT}${FIXTURE_ERROR}")
endif()

execute_process(
    COMMAND "${NSO_TEST_HELPER}" --write-npdm "${NPDM_FIXTURE}"
    RESULT_VARIABLE NPDM_FIXTURE_RESULT
    ERROR_VARIABLE NPDM_FIXTURE_ERROR
)
if(NOT NPDM_FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not create synthetic NPDM: ${NPDM_FIXTURE_ERROR}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${NSO_FIXTURE}" --npdm "${NPDM_FIXTURE}"
    RESULT_VARIABLE NPDM_INSPECT_RESULT
    OUTPUT_VARIABLE NPDM_INSPECT_OUTPUT
    ERROR_VARIABLE NPDM_INSPECT_ERROR
)
if(NOT NPDM_INSPECT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "NPDM-backed NSO inspection failed: ${NPDM_INSPECT_OUTPUT}${NPDM_INSPECT_ERROR}")
endif()
foreach(EXPECTED "Architecture: AArch64 (main.npdm)"
                 "NPDM address space: 64-bit (39-bit address space)"
                 "Program ID: 0100000000010000")
    string(FIND "${NPDM_INSPECT_OUTPUT}" "${EXPECTED}" EXPECTED_POSITION)
    if(EXPECTED_POSITION EQUAL -1)
        message(FATAL_ERROR
            "NPDM-backed inspection is missing '${EXPECTED}':\n${NPDM_INSPECT_OUTPUT}")
    endif()
endforeach()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${NSO_FIXTURE}" --npdm "${NPDM_FIXTURE}" --json
    RESULT_VARIABLE NPDM_JSON_RESULT
    OUTPUT_VARIABLE NPDM_JSON_OUTPUT
    ERROR_VARIABLE NPDM_JSON_ERROR
)
if(NOT NPDM_JSON_RESULT EQUAL 0)
    message(FATAL_ERROR
        "NPDM-backed JSON inspection failed: ${NPDM_JSON_OUTPUT}${NPDM_JSON_ERROR}")
endif()
string(JSON NPDM_PROGRAM_ID_TYPE TYPE "${NPDM_JSON_OUTPUT}" program_id)
string(JSON NPDM_PROGRAM_ID GET "${NPDM_JSON_OUTPUT}" program_id)
if(NOT NPDM_PROGRAM_ID_TYPE STREQUAL "STRING" OR
   NOT NPDM_PROGRAM_ID STREQUAL "0100000000010000")
    message(FATAL_ERROR
        "NPDM-backed JSON inspection has an invalid program ID:\n${NPDM_JSON_OUTPUT}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${NSO_FIXTURE}" --assume-aarch64
    RESULT_VARIABLE INSPECT_RESULT
    OUTPUT_VARIABLE INSPECT_OUTPUT
    ERROR_VARIABLE INSPECT_ERROR
)
if(NOT INSPECT_RESULT EQUAL 0)
    message(FATAL_ERROR "Human-readable NSO inspection failed: ${INSPECT_OUTPUT}${INSPECT_ERROR}")
endif()
foreach(EXPECTED
        "Format: NSO0"
        "Architecture: AArch64 (explicit assumption)"
        "Build ID: 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "text: file=0x100 memory=0x1000 decoded=16 stored=16 compression=none"
        "Mapped text size: 4096"
        "Mapped text SHA-256: d6277c609aa7d18c9d08152a86ae6cac812d224bfa18405a631c56ca9694208a"
        "AArch64 analysis: unavailable (could not validate the conventional AArch64 entry stub and MOD0 header)"
        "ELF64 dynamic analysis: unavailable (MOD0 offset at text+4 is invalid)")
    string(FIND "${INSPECT_OUTPUT}" "${EXPECTED}" EXPECTED_POSITION)
    if(EXPECTED_POSITION EQUAL -1)
        message(FATAL_ERROR "Inspection output is missing '${EXPECTED}':\n${INSPECT_OUTPUT}")
    endif()
endforeach()

set(DYNAMIC_NSO_FIXTURE "${NSO_TEST_DIR}/synthetic-dynamic.nso")
execute_process(
    COMMAND "${NSO_TEST_HELPER}" --write-dynamic-fixture "${DYNAMIC_NSO_FIXTURE}"
    RESULT_VARIABLE DYNAMIC_FIXTURE_RESULT
    ERROR_VARIABLE DYNAMIC_FIXTURE_ERROR
)
if(NOT DYNAMIC_FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not create dynamic NSO: ${DYNAMIC_FIXTURE_ERROR}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${DYNAMIC_NSO_FIXTURE}" --assume-aarch64
    RESULT_VARIABLE DYNAMIC_INSPECT_RESULT
    OUTPUT_VARIABLE DYNAMIC_INSPECT_OUTPUT
    ERROR_VARIABLE DYNAMIC_INSPECT_ERROR
)
if(NOT DYNAMIC_INSPECT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Dynamic NSO inspection failed: ${DYNAMIC_INSPECT_OUTPUT}${DYNAMIC_INSPECT_ERROR}")
endif()
foreach(EXPECTED
        "AArch64 entry: 0x1010"
        "AArch64 blocks: 4"
        "ELF64 dynamic: MOD0=0x6000 table=0x6020 non-null-entries=11 bytes=192"
        "Dynamic symbols: address=0x6180 entry-size=24"
        "Dynamic RELA: address=0x6100 bytes=24 entries=1 entry-size=24"
        "PLT RELA: address=0x6118 bytes=24 entries=1 entry-size=24"
        "ELF64 symbols: entries=4 imports=1 weak-imports=1 exports=2 absolute-definitions=1 relocation-indices=verified"
        "ELF64 relocation plan: writes=2 relative=1 null-symbol=0 module-definitions=0 absolute-definitions=0 external-definitions=0 undefined-weak=1")
    string(FIND "${DYNAMIC_INSPECT_OUTPUT}" "${EXPECTED}" EXPECTED_POSITION)
    if(EXPECTED_POSITION EQUAL -1)
        message(FATAL_ERROR
            "Dynamic inspection is missing '${EXPECTED}':\n${DYNAMIC_INSPECT_OUTPUT}")
    endif()
endforeach()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${DYNAMIC_NSO_FIXTURE}"
            --assume-aarch64 --json
    RESULT_VARIABLE DYNAMIC_JSON_RESULT
    OUTPUT_VARIABLE DYNAMIC_JSON_OUTPUT
    ERROR_VARIABLE DYNAMIC_JSON_ERROR
)
if(NOT DYNAMIC_JSON_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Dynamic JSON inspection failed: ${DYNAMIC_JSON_OUTPUT}${DYNAMIC_JSON_ERROR}")
endif()
string(JSON DYNAMIC_ANALYSIS_TYPE TYPE "${DYNAMIC_JSON_OUTPUT}" dynamic_analysis)
string(JSON DYNAMIC_FORMAT GET "${DYNAMIC_JSON_OUTPUT}" dynamic_analysis format)
string(JSON DYNAMIC_MOD0 GET "${DYNAMIC_JSON_OUTPUT}" dynamic_analysis mod0 address)
string(JSON DYNAMIC_RELA_ENTRIES_TYPE TYPE "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis rela entries)
string(JSON DYNAMIC_RELA_ENTRIES GET "${DYNAMIC_JSON_OUTPUT}" dynamic_analysis rela entries)
string(JSON DYNAMIC_PLT_ENTRIES GET "${DYNAMIC_JSON_OUTPUT}" dynamic_analysis plt_rela entries)
string(JSON DYNAMIC_SYMBOL_ENTRIES_TYPE TYPE "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis symbol_analysis entries)
string(JSON DYNAMIC_SYMBOL_ENTRIES GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis symbol_analysis entries)
string(JSON DYNAMIC_WEAK_IMPORTS GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis symbol_analysis weak_imports)
string(JSON DYNAMIC_ABSOLUTE_DEFINITIONS GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis symbol_analysis absolute_definitions)
string(JSON DYNAMIC_RELOCATION_INDICES GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis symbol_analysis relocation_indices_verified)
string(JSON DYNAMIC_RELOCATION_PLAN_TYPE TYPE "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis relocation_plan)
string(JSON DYNAMIC_RELOCATION_WRITES GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis relocation_plan writes)
string(JSON DYNAMIC_RELOCATION_RELATIVE GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis relocation_plan relative)
string(JSON DYNAMIC_RELOCATION_NULL_SYMBOLS GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis relocation_plan null_symbols)
string(JSON DYNAMIC_RELOCATION_MODULE_DEFINITIONS GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis relocation_plan module_definitions)
string(JSON DYNAMIC_RELOCATION_ABSOLUTE_DEFINITIONS GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis relocation_plan absolute_definitions)
string(JSON DYNAMIC_RELOCATION_EXTERNAL_DEFINITIONS GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis relocation_plan external_definitions)
string(JSON DYNAMIC_RELOCATION_UNDEFINED_WEAK GET "${DYNAMIC_JSON_OUTPUT}"
       dynamic_analysis relocation_plan undefined_weak)
if(NOT DYNAMIC_ANALYSIS_TYPE STREQUAL "OBJECT" OR
   NOT DYNAMIC_FORMAT STREQUAL "ELF64" OR
   NOT DYNAMIC_MOD0 STREQUAL "0x6000" OR
   NOT DYNAMIC_RELA_ENTRIES_TYPE STREQUAL "NUMBER" OR
   NOT DYNAMIC_RELA_ENTRIES EQUAL 1 OR
   NOT DYNAMIC_PLT_ENTRIES EQUAL 1 OR
   NOT DYNAMIC_SYMBOL_ENTRIES_TYPE STREQUAL "NUMBER" OR
   NOT DYNAMIC_SYMBOL_ENTRIES EQUAL 4 OR
   NOT DYNAMIC_WEAK_IMPORTS EQUAL 1 OR
   NOT DYNAMIC_ABSOLUTE_DEFINITIONS EQUAL 1 OR
   NOT DYNAMIC_RELOCATION_INDICES OR
   NOT DYNAMIC_RELOCATION_PLAN_TYPE STREQUAL "OBJECT" OR
   NOT DYNAMIC_RELOCATION_WRITES EQUAL 2 OR
   NOT DYNAMIC_RELOCATION_RELATIVE EQUAL 1 OR
   NOT DYNAMIC_RELOCATION_NULL_SYMBOLS EQUAL 0 OR
   NOT DYNAMIC_RELOCATION_MODULE_DEFINITIONS EQUAL 0 OR
   NOT DYNAMIC_RELOCATION_ABSOLUTE_DEFINITIONS EQUAL 0 OR
   NOT DYNAMIC_RELOCATION_EXTERNAL_DEFINITIONS EQUAL 0 OR
   NOT DYNAMIC_RELOCATION_UNDEFINED_WEAK EQUAL 1)
    message(FATAL_ERROR "Dynamic JSON schema is invalid:\n${DYNAMIC_JSON_OUTPUT}")
endif()

set(LZ4_NSO_FIXTURE "${NSO_TEST_DIR}/synthetic-lz4.nso")
execute_process(
    COMMAND "${NSO_TEST_HELPER}" --write-lz4-fixture "${LZ4_NSO_FIXTURE}"
    RESULT_VARIABLE LZ4_FIXTURE_RESULT
    ERROR_VARIABLE LZ4_FIXTURE_ERROR
)
if(NOT LZ4_FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not create LZ4 NSO: ${LZ4_FIXTURE_ERROR}")
endif()
execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${LZ4_NSO_FIXTURE}"
    RESULT_VARIABLE LZ4_INSPECT_RESULT
    OUTPUT_VARIABLE LZ4_INSPECT_OUTPUT
    ERROR_VARIABLE LZ4_INSPECT_ERROR
)
if(NOT LZ4_INSPECT_RESULT EQUAL 0)
    message(FATAL_ERROR "LZ4 NSO inspection failed: ${LZ4_INSPECT_OUTPUT}${LZ4_INSPECT_ERROR}")
endif()
string(FIND "${LZ4_INSPECT_OUTPUT}" "compression=lz4" LZ4_METADATA_POSITION)
if(LZ4_METADATA_POSITION EQUAL -1)
    message(FATAL_ERROR "LZ4 metadata was not reported:\n${LZ4_INSPECT_OUTPUT}")
endif()
string(FIND "${LZ4_INSPECT_OUTPUT}" "Segments decoded: yes" LZ4_DECODED_POSITION)
string(FIND "${LZ4_INSPECT_OUTPUT}" "requires an LZ4 decompressor" LZ4_UNAVAILABLE_POSITION)
if(LZ4_DECODED_POSITION EQUAL -1 AND LZ4_UNAVAILABLE_POSITION EQUAL -1)
    message(FATAL_ERROR "LZ4 decoding was neither successful nor clearly unavailable:\n${LZ4_INSPECT_OUTPUT}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${NSO_FIXTURE}" --json
    RESULT_VARIABLE JSON_RESULT
    OUTPUT_VARIABLE JSON_OUTPUT
    ERROR_VARIABLE JSON_ERROR
)
if(NOT JSON_RESULT EQUAL 0)
    message(FATAL_ERROR "JSON NSO inspection failed: ${JSON_OUTPUT}${JSON_ERROR}")
endif()
foreach(EXPECTED
        "\"format\": \"NSO0\""
        "\"architecture\": null"
        "\"build_id\": \"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\""
        "\"decoded_size\": 16")
    string(FIND "${JSON_OUTPUT}" "${EXPECTED}" EXPECTED_POSITION)
    if(EXPECTED_POSITION EQUAL -1)
        message(FATAL_ERROR "JSON output is missing '${EXPECTED}':\n${JSON_OUTPUT}")
    endif()
endforeach()

string(JSON PROGRAM_ID_TYPE TYPE "${JSON_OUTPUT}" program_id)
string(JSON MAPPED_TEXT_SIZE_TYPE TYPE "${JSON_OUTPUT}" mapped_text_size)
string(JSON MAPPED_TEXT_SIZE GET "${JSON_OUTPUT}" mapped_text_size)
string(JSON MAPPED_TEXT_SHA256_TYPE TYPE "${JSON_OUTPUT}" mapped_text_sha256)
string(JSON MAPPED_TEXT_SHA256 GET "${JSON_OUTPUT}" mapped_text_sha256)
if(NOT PROGRAM_ID_TYPE STREQUAL "NULL")
    message(FATAL_ERROR "NPDM-less JSON program ID must be null:\n${JSON_OUTPUT}")
endif()
if(NOT MAPPED_TEXT_SIZE_TYPE STREQUAL "NUMBER" OR NOT MAPPED_TEXT_SIZE EQUAL 4096)
    message(FATAL_ERROR "JSON mapped text size is invalid:\n${JSON_OUTPUT}")
endif()
if(NOT MAPPED_TEXT_SHA256_TYPE STREQUAL "STRING" OR
   NOT MAPPED_TEXT_SHA256 STREQUAL
       "d6277c609aa7d18c9d08152a86ae6cac812d224bfa18405a631c56ca9694208a")
    message(FATAL_ERROR "JSON mapped text SHA-256 is invalid:\n${JSON_OUTPUT}")
endif()

string(JSON UNKNOWN_DYNAMIC_TYPE TYPE "${JSON_OUTPUT}" dynamic_analysis)
if(NOT UNKNOWN_DYNAMIC_TYPE STREQUAL "NULL")
    message(FATAL_ERROR "Unknown-architecture dynamic analysis must be null:\n${JSON_OUTPUT}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${NSO_FIXTURE}" --assume-aarch64 --json
    RESULT_VARIABLE UNAVAILABLE_JSON_RESULT
    OUTPUT_VARIABLE UNAVAILABLE_JSON_OUTPUT
    ERROR_VARIABLE UNAVAILABLE_JSON_ERROR
)
if(NOT UNAVAILABLE_JSON_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Unavailable dynamic JSON inspection became fatal: "
        "${UNAVAILABLE_JSON_OUTPUT}${UNAVAILABLE_JSON_ERROR}")
endif()
string(JSON UNAVAILABLE_DYNAMIC_TYPE TYPE "${UNAVAILABLE_JSON_OUTPUT}" dynamic_analysis)
string(JSON UNAVAILABLE_DYNAMIC_ERROR GET "${UNAVAILABLE_JSON_OUTPUT}" dynamic_analysis error)
string(FIND "${UNAVAILABLE_DYNAMIC_ERROR}" "MOD0" UNAVAILABLE_MOD0_POSITION)
if(NOT UNAVAILABLE_DYNAMIC_TYPE STREQUAL "OBJECT" OR UNAVAILABLE_MOD0_POSITION EQUAL -1)
    message(FATAL_ERROR
        "Unavailable dynamic JSON analysis lacks its diagnostic object:\n${UNAVAILABLE_JSON_OUTPUT}")
endif()
