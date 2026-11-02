cmake_minimum_required(VERSION 3.20)
project(recompiler_fpx LANGUAGES C CXX)
# FPX_EXTRA_C_FLAGS: extra flags for the generated op files and the driver, for
# the compiler matrix (e.g. "-mfma -mavx2", "-march=x86-64-v3 -ffp-contract=fast").
set(FPX_EXTRA_C_FLAGS "" CACHE STRING "extra C flags for the generated code")

add_executable(fpx_gen fpx_gen.cpp)
target_compile_features(fpx_gen PRIVATE cxx_std_20)
target_include_directories(fpx_gen PRIVATE "${SUYU_SOURCE}/src" "${CMAKE_CURRENT_SOURCE_DIR}")

set(gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(gen_files "${gen}/rt_soft.h" "${gen}/rt_soft.c" "${gen}/ops_soft.c" "${gen}/hw.c"
    "${gen}/rt_fpx.h" "${gen}/ops_fpx.c" "${gen}/rt_nokeep.h" "${gen}/ops_nokeep.c"
    "${gen}/rt_nomid.h" "${gen}/ops_nomid.c" "${gen}/rt_shadow.h" "${gen}/ops_shadow.c"
    "${gen}/shadow_rt.c" "${gen}/fpx_exact.c" "${gen}/rt_nofz.h" "${gen}/ops_nofz.c")
add_custom_command(OUTPUT ${gen_files} COMMAND fpx_gen "${gen}" DEPENDS fpx_gen VERBATIM)

# The runtime as the static host builds it, for recomp_fp_estimate.
add_library(fpx_runtime STATIC "${gen}/rt_soft.c")
target_include_directories(fpx_runtime PRIVATE "${gen}")
target_compile_definitions(fpx_runtime PRIVATE SUYU_HOSTED_RECOMP=1 RECOMP_STATIC_HOST=1)

set(driver_sources driver.c fpx_env.cpp "${gen}/ops_soft.c" "${gen}/ops_fpx.c"
    "${gen}/ops_nokeep.c" "${gen}/ops_nomid.c" "${gen}/ops_nofz.c" "${gen}/ops_shadow.c" "${gen}/shadow_rt.c"
    "${gen}/fpx_exact.c")
# fpx_env.cpp: the host's own FP-mode check (core/arm/recomp/guest_fp_env.h).
set(driver_defs FPX_HAVE_FPX=1 FPX_HAVE_ENV=1)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$" AND NOT MSVC)
  list(APPEND driver_sources "${gen}/hw.c")
  list(APPEND driver_defs FPX_HAVE_HW=1)
endif()
add_executable(fpx_driver ${driver_sources})
target_include_directories(fpx_driver PRIVATE "${gen}" "${CMAKE_CURRENT_SOURCE_DIR}" "${SUYU_SOURCE}/src")
target_compile_features(fpx_driver PRIVATE c_std_11 cxx_std_20)
target_compile_definitions(fpx_driver PRIVATE ${driver_defs})
target_link_libraries(fpx_driver PRIVATE fpx_runtime)
if(MSVC)
  target_compile_options(fpx_driver PRIVATE /bigobj /fp:precise)
else()
  target_compile_options(fpx_driver PRIVATE -fno-fast-math)
  target_link_libraries(fpx_driver PRIVATE m)
endif()
separate_arguments(fpx_extra NATIVE_COMMAND "${FPX_EXTRA_C_FLAGS}")
target_compile_options(fpx_driver PRIVATE "$<$<COMPILE_LANGUAGE:C>:${fpx_extra}>")
