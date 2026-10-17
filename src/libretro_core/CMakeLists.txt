# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(suyu_libretro SHARED
    libretro.h
    retro_core.cpp
    retro_emu_window.cpp
    retro_emu_window.h
)

target_link_libraries(suyu_libretro PRIVATE common core input_common frontend_common audio_core network)
target_link_libraries(suyu_libretro PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)
# The Vulkan types of RetroArch's HW render interface (libretro_vulkan.h)
target_link_libraries(suyu_libretro PRIVATE Vulkan::Headers)

if (ANDROID)
    target_link_options(suyu_libretro PRIVATE "-Wl,-Bsymbolic")
endif()

set_target_properties(suyu_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "suyu_libretro"
)

create_target_directory_groups(suyu_libretro)

if (NOT ANDROID)
    add_executable(libretro_tas_frame_smoke EXCLUDE_FROM_ALL
        tas_frame_smoke.cpp retro_emu_window.cpp)
    target_link_libraries(libretro_tas_frame_smoke PRIVATE
        common core input_common frontend_common audio_core network
        ${PLATFORM_LIBRARIES} Threads::Threads)
endif()
