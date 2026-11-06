// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace suyu::recomp::tool {

bool ComputeSha256(std::span<const std::uint8_t> source,
                   std::array<std::uint8_t, 0x20>& digest);

} // namespace suyu::recomp::tool
