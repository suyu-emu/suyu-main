// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "input_common/drivers/tas_input.h"
#include "libretro_core/retro_emu_window.h"

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("suyu-tas-frame-smoke-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    std::ofstream(directory / "script0-1.txt")
        << "0 KEY_A 0;0 0;0\n1 NONE 0;0 0;0\n2 KEY_B 0;0 0;0\n";
    Common::FS::SetSuyuPath(Common::FS::SuyuPath::TASDir, directory);
    Settings::values.tas_enable.SetValue(true);
    Settings::values.tas_loop.SetValue(false);
    // Exercise the frontend hook without initializing the full input subsystem
    // or logger, both of which may use the machine's real profile.
    InputCommon::TasInput::Tas tas{"tas-smoke"};
    tas.BeginBootSession(InputCommon::TasInput::TasBootMode::Playback);
    LibretroCore::RetroEmuWindow window;
    window.SetTasPlayback(&tas);
    bool success = std::get<1>(tas.GetStatus()) == 0;
    for (std::size_t cursor = 1; cursor <= 3; ++cursor) {
        window.OnFrameDisplayed();
        success &= std::get<1>(tas.GetStatus()) == cursor;
    }
    window.OnFrameDisplayed();
    const auto [generation, commands, looping] = tas.GetCompletionStatus();
    success &= generation == 1 && commands == 3 && !looping;
    success &= std::get<0>(tas.GetStatus()) == InputCommon::TasInput::TasState::Stopped;
    window.OnFrameDisplayed();
    success &= std::get<0>(tas.GetCompletionStatus()) == 1;
    window.SetTasPlayback(nullptr);
    tas.BeginBootSession(InputCommon::TasInput::TasBootMode::None);
    window.OnFrameDisplayed();
    success &= std::get<1>(tas.GetStatus()) == 0;
    success &= std::get<0>(tas.GetStatus()) == InputCommon::TasInput::TasState::Stopped;
    tas.BeginBootSession(InputCommon::TasInput::TasBootMode::Playback);
    window.SetTasPlayback(&tas);
    window.OnFrameDisplayed();
    success &= std::get<1>(tas.GetStatus()) == 1;
    window.SetTasPlayback(nullptr);
    tas.BeginBootSession(InputCommon::TasInput::TasBootMode::None);
    std::filesystem::remove(directory / "script0-1.txt");
    std::filesystem::remove(directory);
    std::cout << "TAS_FRAME_AND_EOF_SMOKE=" << (success ? "PASS" : "FAIL") << '\n';
    std::cout << "TAS_SESSION_RESET_AND_REARM_SMOKE=" << (success ? "PASS" : "FAIL") << '\n';
    return success ? 0 : 1;
}
