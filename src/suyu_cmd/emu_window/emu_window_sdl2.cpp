// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <set>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
// SDL3 removed these constants; define compat shims
static constexpr Uint8 SDL_PRESSED = 1;
static constexpr Uint8 SDL_RELEASED = 0;

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/drivers/mouse.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/main.h"
#include "common/param_package.h"
#include "common/settings_input.h"
#include "suyu_cmd/controller_assign.h"
#include "suyu_cmd/emu_window/emu_window_sdl2.h"
#include "suyu_cmd/native_status.h"
#include "suyu_cmd/sdl_config.h"
#include "suyu_cmd/suyu_icon.h"
#include "video_core/gpu.h"
#include "video_core/shader_notify.h"

namespace {
constexpr u64 kStatusRefreshMs = 750;
constexpr u64 kControlsSaveMs = 2000;
constexpr Sint32 kEventWaitSliceMs = 250;

// The controllers among the input devices: gamepads SDL recognises and whatever suyu's own
// Joy-Con / Pro Controller driver lists. SDL also lists wheels, flight sticks and virtual or
// RGB devices as joysticks, and none of those should take a player. The input backend names
// an SDL device by its GUID with the name checksum cleared. @p sdl_gamepads, when given, gets
// how many gamepads SDL itself sees.
std::vector<Common::ParamPackage> ControllerPads(const std::vector<Common::ParamPackage>& devices,
                                                 std::size_t* sdl_gamepads = nullptr) {
    std::set<std::string> gamepad_guids;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) {
        SDL_GUID guid = SDL_GetJoystickGUIDForID(ids[i]);
        guid.data[2] = guid.data[3] = 0;
        char text[33]{};
        SDL_GUIDToString(guid, text, sizeof(text));
        gamepad_guids.insert(text);
    }
    SDL_free(ids);
    if (sdl_gamepads) {
        *sdl_gamepads = gamepad_guids.size();
    }
    std::vector<Common::ParamPackage> pads;
    for (const auto& device : devices) {
        const std::string engine = device.Get("engine", "");
        if ((engine == "sdl" && gamepad_guids.contains(device.Get("guid", ""))) ||
            engine == "joycon") {
            pads.push_back(device);
        }
    }
    return pads;
}

// The input backend's own default mapping for a controller, as the settings screen's
// auto-map writes it; for gamepads it includes Home and, where the pad has one, Capture.
void MapDefault(InputCommon::InputSubsystem& input, const Common::ParamPackage& pad,
                Settings::PlayerInput& player) {
    for (const auto& [button, param] : input.GetButtonMappingForDevice(pad)) {
        player.buttons[button] = param.Serialize();
    }
    for (const auto& [analog, param] : input.GetAnalogMappingForDevice(pad)) {
        player.analogs[analog] = param.Serialize();
    }
    for (const auto& [motion, param] : input.GetMotionMappingForDevice(pad)) {
        player.motions[motion] = param.Serialize();
    }
}
} // namespace

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <array>
#include <filesystem>
#include <iterator>
#include <vector>
#include <system_error>
#include <string>
#include <vector>

// ── F12 debug panel ─────────────────────────────────────────────────────────
// A real interactive window (not a message box): live status text refreshed on
// a timer, a list of the mod folders currently visible to the patch manager,
// and buttons that open the folders this build actually uses. Deliberately
// carries no emulator branding — an exported game shows the game's own name.
namespace {

constexpr int kIdStatus = 1001;
constexpr int kIdMods = 1002;
constexpr int kIdOpenUser = 1003;
constexpr int kIdOpenMods = 1004;
constexpr int kIdOpenKeys = 1005;
constexpr int kIdClose = 1006;
constexpr int kIdDevices = 1007;
constexpr int kIdApplyPad = 1008;
constexpr int kIdKeyboard = 1009;
constexpr int kIdRescanPads = 1010;
constexpr int kIdBindList = 1011;
constexpr int kIdBindOne = 1012;
constexpr int kIdClearOne = 1013;
constexpr int kIdCombineJoycons = 1014;
constexpr int kIdSplitJoycons = 1015;
constexpr int kIdResScale = 1016;
constexpr UINT_PTR kTimer = 1;

// Same choices, in the same order, as the Qt frontend's View -> Resolution Scale submenu
// (src/suyu/main.cpp, PopulateResolutionScaleMenu). Kept as one array so the F12 panel and
// Qt never drift apart on what "2X" etc. means.
constexpr std::array<std::pair<Settings::ResolutionSetup, const wchar_t*>, 13> kResScaleEntries{{
    {Settings::ResolutionSetup::Res1_4X, L"0.25X (180p/270p) [EXPERIMENTAL]"},
    {Settings::ResolutionSetup::Res1_2X, L"0.5X (360p/540p) [EXPERIMENTAL]"},
    {Settings::ResolutionSetup::Res3_4X, L"0.75X (540p/810p) [EXPERIMENTAL]"},
    {Settings::ResolutionSetup::Res1X, L"1X (720p/1080p)"},
    {Settings::ResolutionSetup::Res5_4X, L"1.25X (900p/1350p) [EXPERIMENTAL]"},
    {Settings::ResolutionSetup::Res3_2X, L"1.5X (1080p/1620p) [EXPERIMENTAL]"},
    {Settings::ResolutionSetup::Res2X, L"2X (1440p/2160p)"},
    {Settings::ResolutionSetup::Res3X, L"3X (2160p/3240p)"},
    {Settings::ResolutionSetup::Res4X, L"4X (2880p/4320p)"},
    {Settings::ResolutionSetup::Res5X, L"5X (3600p/5400p)"},
    {Settings::ResolutionSetup::Res6X, L"6X (4320p/6480p)"},
    {Settings::ResolutionSetup::Res7X, L"7X (5040p/7560p)"},
    {Settings::ResolutionSetup::Res8X, L"8X (5760p/8640p)"},
}};

std::filesystem::path DevExeDir() {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    return std::filesystem::path(exe_path).parent_path();
}

std::wstring DevKeysDir() {
    return Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir).wstring();
}

void DevOpen(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    ShellExecuteW(nullptr, L"explore", p.wstring().c_str(), nullptr, nullptr, SW_SHOW);
}

struct DevPanelState {
    Core::System* system{};
    InputCommon::InputSubsystem* input{};
    HWND status{};
    HWND mods{};
    HWND devices{};
    HWND binds{};
    HWND combine{};
    HWND split{};
    HWND res_scale{};
    std::vector<Common::ParamPackage> device_list;
};

// Shows the current value; see DevApplyResScale for the write side.
void DevRefreshResScale(DevPanelState& st) {
    const auto current = Settings::values.resolution_setup.GetValue();
    int index = 0;
    for (std::size_t i = 0; i < kResScaleEntries.size(); ++i) {
        if (kResScaleEntries[i].first == current) {
            index = static_cast<int>(i);
            break;
        }
    }
    SendMessageW(st.res_scale, CB_SETCURSEL, index, 0);
}

// Same effect as the Qt frontend's OnResolutionScaleSelected while a game is running: the
// setting is changed and saved right away. Existing render targets already on the GPU keep
// their current size (see GMainWindow::RestartForResolutionScale in src/suyu/main.cpp for
// why - rescaled images refuse in-place reallocation and the pipeline cache does not key on
// resolution), so - exactly like Qt - the new scale takes full effect the next time this
// package is started, not on the frame after picking it.
void DevApplyResScale(DevPanelState& st) {
    const int index = static_cast<int>(SendMessageW(st.res_scale, CB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<std::size_t>(index) >= kResScaleEntries.size()) {
        return;
    }
    const auto setup = kResScaleEntries[index].first;
    if (Settings::values.resolution_setup.GetValue() == setup) {
        return;
    }
    Settings::values.resolution_setup.SetValue(setup);
    Settings::UpdateRescalingInfo();
    if (st.system != nullptr) {
        st.system->ApplySettings();
    }
    SaveNativeControls();
}

// Combine shows while a pair's halves are on two players, Split while a pair is on one.
void DevRefreshJoyconButtons(DevPanelState& st) {
    if (st.input == nullptr) {
        return;
    }
    const auto pads = ControllerPads(st.input->GetInputDevices());
    auto& players = Settings::values.players.GetValue();
    ShowWindow(st.combine,
               SuyuCmd::CombineJoycons(players, pads, SdlConfig::default_buttons, {}) ? SW_SHOW
                                                                                    : SW_HIDE);
    ShowWindow(st.split,
               SuyuCmd::SplitJoycons(players, pads, SdlConfig::default_buttons, {}) ? SW_SHOW
                                                                                  : SW_HIDE);
}

void DevJoyconAction(DevPanelState& st, bool combine) {
    if (st.input == nullptr) {
        return;
    }
    const auto pads = ControllerPads(st.input->GetInputDevices());
    auto& players = Settings::values.players.GetValue();
    const auto map_pad = [&st](const Common::ParamPackage& pad, Settings::PlayerInput& player) {
        MapDefault(*st.input, pad, player);
    };
    const auto slot =
        combine ? SuyuCmd::CombineJoycons(players, pads, SdlConfig::default_buttons, map_pad)
                : SuyuCmd::SplitJoycons(players, pads, SdlConfig::default_buttons, map_pad);
    if (!slot) {
        MessageBoxW(nullptr, L"No Joy-Cons to change right now.", L"Controls",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    // The player's own choice from here on: auto-assign leaves the controls alone.
    if (g_export_package) {
        SdlConfig::auto_assign_controllers = false;
    }
    if (st.system != nullptr) {
        st.system->HIDCore().ReloadInputDevices();
    }
    SaveNativeControls();
    const std::wstring text =
        combine ? L"Joy-Cons combined into Player " + std::to_wstring(*slot + 1) + L"."
                : L"Joy-Cons split: the left one is Player " + std::to_wstring(*slot + 1) +
                      L", the right one the next player.";
    MessageBoxW(nullptr, text.c_str(), L"Controls", MB_OK | MB_ICONINFORMATION);
    DevRefreshJoyconButtons(st);
}

// Per-button remapping. Auto-map covers the common case; this covers the rest -
// pick the entry, press the input you want, done. Same "press what you want"
// flow the emulator's own input dialog uses, driven off the input backend's
// polling API rather than a second mapping implementation.
void DevRefreshBinds(DevPanelState& st) {
    const int sel = static_cast<int>(SendMessageW(st.binds, LB_GETCURSEL, 0, 0));
    SendMessageW(st.binds, LB_RESETCONTENT, 0, 0);
    const auto& player = Settings::values.players.GetValue()[0];
    const auto add = [&](const char* label, const std::string& param) {
        Common::ParamPackage pkg{param};
        std::string shown = param.empty() ? "(unset)" : pkg.Get("display", param);
        if (shown.size() > 60) {
            shown.resize(60);
        }
        const std::string line = std::string(label) + "  =  " + shown;
        const std::wstring wide(line.begin(), line.end());
        SendMessageW(st.binds, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
    };
    for (std::size_t i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        add(Settings::NativeButton::mapping[i], player.buttons[i]);
    }
    for (std::size_t i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        add(Settings::NativeAnalog::mapping[i], player.analogs[i]);
    }
    if (sel >= 0) {
        SendMessageW(st.binds, LB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    }
}

void DevBindSelected(DevPanelState& st, bool clear) {
    const int sel = static_cast<int>(SendMessageW(st.binds, LB_GETCURSEL, 0, 0));
    constexpr int kButtonCount = static_cast<int>(Settings::NativeButton::NumButtons);
    constexpr int kAnalogCount = static_cast<int>(Settings::NativeAnalog::NumAnalogs);
    if (sel < 0 || sel >= kButtonCount + kAnalogCount || st.input == nullptr) {
        return;
    }
    const bool is_analog = sel >= kButtonCount;
    auto& player = Settings::values.players.GetValue()[0];

    if (clear) {
        if (is_analog) {
            player.analogs[sel - kButtonCount].clear();
        } else {
            player.buttons[sel].clear();
        }
        DevRefreshBinds(st);
        // A choice made here is the player's own; auto-assign leaves it alone.
        if (g_export_package) {
            SdlConfig::auto_assign_controllers = false;
        }
        SaveNativeControls();
        if (st.system != nullptr) {
            st.system->HIDCore().ReloadInputDevices();
        }
        return;
    }

    st.input->BeginMapping(is_analog ? InputCommon::Polling::InputType::Stick
                                     : InputCommon::Polling::InputType::Button);
    // Poll rather than block: the panel owns the message loop, and a modal
    // "press something" dialog with no way out is worse than a timeout.
    Common::ParamPackage captured;
    const DWORD deadline = GetTickCount() + 5000;
    while (GetTickCount() < deadline) {
        SDL_PumpEvents();
        captured = st.input->GetNextInput();
        if (captured.Has("engine")) {
            break;
        }
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
    st.input->StopMapping();

    if (!captured.Has("engine")) {
        MessageBoxW(nullptr, L"No input detected - nothing changed.", L"Controls",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (is_analog) {
        player.analogs[sel - kButtonCount] = captured.Serialize();
    } else {
        player.buttons[sel] = captured.Serialize();
    }
    player.connected = true;
    if (st.system != nullptr) {
        st.system->HIDCore().ReloadInputDevices();
    }
    if (g_export_package) {
        SdlConfig::auto_assign_controllers = false;
    }
    SaveNativeControls();
    DevRefreshBinds(st);
}

// Controller setup, done the way a player expects: pick the pad from a list and
// press one button. The full per-button remapper belongs in the emulator's own
// UI - what a shipped game build needs is for a plugged-in pad to just work,
// and a way back to keyboard when it does not.
void DevRefreshDevices(DevPanelState& st) {
    SDL_PumpEvents();
    const int previous = static_cast<int>(SendMessageW(st.devices, CB_GETCURSEL, 0, 0));
    const auto selected = previous >= 0 && static_cast<std::size_t>(previous) < st.device_list.size()
                              ? st.device_list[previous].Serialize()
                              : std::string{};
    SendMessageW(st.devices, CB_RESETCONTENT, 0, 0);
    st.device_list.clear();
    if (st.input == nullptr) {
        return;
    }
    for (const auto& device : st.input->GetInputDevices()) {
        const std::string engine = device.Get("engine", "");
        if (engine != "sdl" && engine != "joycon" && engine != "gcpad") {
            continue;
        }
        const std::string name = device.Get("display", device.Get("class", "Unknown"));
        st.device_list.push_back(device);
        const std::wstring wide(name.begin(), name.end());
        SendMessageW(st.devices, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
    }
    if (st.device_list.empty()) {
        bool saved_pad = false;
        for (const auto& binding : Settings::values.players.GetValue()[0].buttons) {
            Common::ParamPackage existing{binding};
            if (existing.Get("engine", "") == "sdl") {
                saved_pad = true;
                break;
            }
        }
        SendMessageW(st.devices, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(saved_pad
                         ? L"(saved controller disconnected - reconnect and rescan)"
                         : L"(no controller detected - plug one in and rescan)"));
    }
    int preferred = 0;
    if (!selected.empty()) {
        for (std::size_t i = 0; i < st.device_list.size(); ++i) {
            if (st.device_list[i].Serialize() == selected) {
                preferred = static_cast<int>(i);
                break;
            }
        }
    } else {
        const auto& buttons = Settings::values.players.GetValue()[0].buttons;
        for (const auto& binding : buttons) {
            Common::ParamPackage existing{binding};
            if (!existing.Has("guid") || !existing.Has("port")) {
                continue;
            }
            for (std::size_t i = 0; i < st.device_list.size(); ++i) {
                if (st.device_list[i].Get("guid", "") == existing.Get("guid", "") &&
                    st.device_list[i].Get("port", -1) == existing.Get("port", -2)) {
                    preferred = static_cast<int>(i);
                    break;
                }
            }
            break;
        }
    }
    SendMessageW(st.devices, CB_SETCURSEL, preferred, 0);
}

void DevApplyPadMapping(DevPanelState& st) {
    const auto index = static_cast<std::size_t>(SendMessageW(st.devices, CB_GETCURSEL, 0, 0));
    if (st.input == nullptr || index >= st.device_list.size()) {
        MessageBoxW(nullptr, L"No controller selected.", L"Controls", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const auto& device = st.device_list[index];
    // GetValue() hands back a reference to the live array, so the mappings are
    // written straight into the setting.
    auto& player = Settings::values.players.GetValue()[0];
    for (const auto& [button, param] : st.input->GetButtonMappingForDevice(device)) {
        player.buttons[button] = param.Serialize();
    }
    for (const auto& [analog, param] : st.input->GetAnalogMappingForDevice(device)) {
        player.analogs[analog] = param.Serialize();
    }
    for (const auto& [motion, param] : st.input->GetMotionMappingForDevice(device)) {
        player.motions[motion] = param.Serialize();
    }
    player.connected = true;
    if (st.system != nullptr) {
        st.system->HIDCore().ReloadInputDevices();
    }
    if (g_export_package) {
        SdlConfig::auto_assign_controllers = false;
    }
    SaveNativeControls();
    MessageBoxW(nullptr, L"Controller mapped to Player 1.", L"Controls",
                MB_OK | MB_ICONINFORMATION);
}

void DevApplyKeyboardMapping(DevPanelState& st) {
    // Same layout the emulator ships as its keyboard default.
    static constexpr std::array<int, Settings::NativeButton::NumButtons> kButtons = {
        SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_Z, SDL_SCANCODE_X,
        SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_M, SDL_SCANCODE_N,
        SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_B,
    };
    static constexpr std::array<std::array<int, 4>, Settings::NativeAnalog::NumAnalogs> kAnalogs{{
        {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT},
        {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L},
    }};
    // GetValue() hands back a reference to the live array, so the mappings are
    // written straight into the setting.
    auto& player = Settings::values.players.GetValue()[0];
    for (std::size_t i = 0; i < kButtons.size() && i < player.buttons.size(); ++i) {
        player.buttons[i] = InputCommon::GenerateKeyboardParam(kButtons[i]);
    }
    for (std::size_t i = 0; i < kAnalogs.size() && i < player.analogs.size(); ++i) {
        player.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
            kAnalogs[i][0], kAnalogs[i][1], kAnalogs[i][2], kAnalogs[i][3], 0, 0.5f);
    }
    player.connected = true;
    if (st.system != nullptr) {
        st.system->HIDCore().ReloadInputDevices();
    }
    if (g_export_package) {
        SdlConfig::auto_assign_controllers = false;
    }
    SaveNativeControls();
    MessageBoxW(nullptr, L"Keyboard controls restored for Player 1.", L"Controls",
                MB_OK | MB_ICONINFORMATION);
}

std::wstring DevStatusText(Core::System& system) {
    // Read the shared snapshot rather than sampling. GetAndResetPerfStats
    // clears the counters as it reads them, so a second caller here would take
    // half the frames away from whoever owns the sample.
    SuyuCmd::NativeStatusSnapshot snap{};
    const bool have_snapshot = SuyuCmd::TryGetNativeStatusSnapshot(snap);
    if (!have_snapshot) {
        snap.title_id = system.GetApplicationProcessProgramID();
    }

    const auto backend = SuyuCmd::ClassifyGameBackend(snap);
    const std::string backend_line =
        std::string(SuyuCmd::NativeBackendName(backend)) + SuyuCmd::GameBackendQualifier(snap);

    wchar_t fps_line[160];
    if (!have_snapshot || !snap.perf_available) {
        swprintf(fps_line, std::size(fps_line), L"FPS:          (no sample yet)");
    } else if (snap.speed_meaningful) {
        swprintf(fps_line, std::size(fps_line), L"FPS:          %.1f   Speed: %.0f%%   CPU work: %.2f ms",
                 snap.average_game_fps, snap.emulation_speed * 100.0, snap.frametime_ms);
    } else {
        // Multicore: guest time follows the host clock, so the speed would always read 100%.
        swprintf(fps_line, std::size(fps_line), L"FPS:          %.1f   CPU work: %.2f ms",
                 snap.average_game_fps, snap.frametime_ms);
    }

    wchar_t applet_line[192];
    if (snap.applet_running) {
        const std::wstring applet_name(snap.applet_name.begin(), snap.applet_name.end());
        swprintf(applet_line, std::size(applet_line), L"\r\nApplet:       %s - %hs", applet_name.c_str(),
                 SuyuCmd::NativeBackendName(snap.applet_backend));
    } else {
        swprintf(applet_line, std::size(applet_line), L"");
    }

    wchar_t buf[2560];
    const auto exe_dir = DevExeDir();
    swprintf(buf, std::size(buf),
             L"Title:        %hs\r\n"
             L"Title ID:     %016llX\r\n"
             L"Version:      %hs\r\n"
             L"%s\r\n"
             L"CPU backend:  %hs\r\n"
             L"JIT trans.:   %llu (requested strict=%hs)\r\n"
             L"%s\r\n"
             L"F12:          Controls panel\r\n"
             L"\r\n"
             L"User data:    %s\r\n"
             L"Mods:         %s\r\n"
             L"Keys:         %s\r\n",
             snap.game_name.empty() ? "(not loaded)" : snap.game_name.c_str(),
             static_cast<unsigned long long>(snap.title_id),
             snap.display_version.empty() ? "(unknown)" : snap.display_version.c_str(), fps_line,
             backend_line.c_str(), static_cast<unsigned long long>(snap.jit_transitions),
             snap.strict_requested ? "yes" : "no", applet_line,
             (exe_dir / L"user").wstring().c_str(), (exe_dir / L"mods").wstring().c_str(),
             DevKeysDir().c_str());
    return buf;
}

void DevRefreshMods(HWND list) {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    const auto mods_dir = DevExeDir() / L"mods";
    bool any = false;
    std::error_code ec;
    if (std::filesystem::is_directory(mods_dir, ec)) {
        for (const auto& tid : std::filesystem::directory_iterator(mods_dir, ec)) {
            if (!tid.is_directory()) {
                continue;
            }
            for (const auto& mod : std::filesystem::directory_iterator(tid.path(), ec)) {
                const std::wstring entry =
                    tid.path().filename().wstring() + L"  /  " + mod.path().filename().wstring();
                SendMessageW(list, LB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(entry.c_str()));
                any = true;
            }
        }
    }
    if (!any) {
        SendMessageW(list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"(none - drop <title id>/<mod name>/ into mods/)"));
    }
}

LRESULT CALLBACK DevPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<DevPanelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_TIMER:
        if (st != nullptr && st->system != nullptr) {
            const bool benchmark_owns_stats = std::getenv("SUYU_CMD_PERF_SAMPLE") != nullptr;
            SuyuCmd::StoreNativeStatusSnapshot(
                SuyuCmd::SampleNativeStatus(*st->system, !benchmark_owns_stats));
            SetWindowTextW(st->status, DevStatusText(*st->system).c_str());
            DevRefreshJoyconButtons(*st);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdOpenUser:
            DevOpen(DevExeDir() / L"user");
            return 0;
        case kIdOpenMods:
            DevOpen(DevExeDir() / L"mods");
            return 0;
        case kIdOpenKeys:
            DevOpen(DevKeysDir());
            return 0;
        case kIdClose:
            DestroyWindow(hwnd);
            return 0;
        case kIdRescanPads:
            if (st != nullptr) {
                DevRefreshDevices(*st);
            }
            return 0;
        case kIdApplyPad:
            if (st != nullptr) {
                DevApplyPadMapping(*st);
                DevRefreshBinds(*st);
            }
            return 0;
        case kIdKeyboard:
            if (st != nullptr) {
                DevApplyKeyboardMapping(*st);
                DevRefreshBinds(*st);
            }
            return 0;
        case kIdBindOne:
            if (st != nullptr) {
                DevBindSelected(*st, false);
            }
            return 0;
        case kIdClearOne:
            if (st != nullptr) {
                DevBindSelected(*st, true);
            }
            return 0;
        case kIdCombineJoycons:
        case kIdSplitJoycons:
            if (st != nullptr) {
                DevJoyconAction(*st, LOWORD(wp) == kIdCombineJoycons);
                DevRefreshBinds(*st);
            }
            return 0;
        case kIdBindList:
            if (HIWORD(wp) == LBN_DBLCLK && st != nullptr) {
                DevBindSelected(*st, false);
            }
            return 0;
        case kIdMods:
            if (HIWORD(wp) == LBN_DBLCLK && st != nullptr) {
                DevRefreshMods(st->mods);
            }
            return 0;
        case kIdResScale:
            if (HIWORD(wp) == CBN_SELCHANGE && st != nullptr) {
                DevApplyResScale(*st);
            }
            return 0;
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimer);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowDevMenu(Core::System& system, InputCommon::InputSubsystem* input) {
    const auto previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    static bool registered = false;
    static const wchar_t* kClass = L"SuyuGameDebugPanel";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DevPanelProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        wc.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        RegisterClassExW(&wc);
        registered = true;
    }

    SuyuCmd::NativeStatusSnapshot panel_snapshot{};
    if (!SuyuCmd::TryGetNativeStatusSnapshot(panel_snapshot)) {
        panel_snapshot = SuyuCmd::SampleNativeStatus(system, false);
    }
    const std::string& game_name = panel_snapshot.game_name;
    const std::wstring title =
        (game_name.empty() ? std::wstring(L"Game") : std::wstring(game_name.begin(), game_name.end())) +
        L" - Debug Panel (F12)";

    const HWND hwnd = CreateWindowExW(0, kClass, title.c_str(),
                                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                      CW_USEDEFAULT, 720, 820, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        if (previous_dpi != nullptr) {
            SetThreadDpiAwarenessContext(previous_dpi);
        }
        return;
    }

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    DevPanelState state{};
    state.system = &system;
    state.input = input;
    state.status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                       ES_AUTOVSCROLL | WS_VSCROLL,
                                   10, 10, 690, 190, hwnd,
                                   reinterpret_cast<HMENU>(kIdStatus), inst, nullptr);
    CreateWindowExW(0, L"STATIC", L"Mods discovered under mods/ (double-click to rescan):",
                    WS_CHILD | WS_VISIBLE, 12, 208, 500, 18, hwnd, nullptr, inst, nullptr);
    state.mods = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 10, 228, 690,
                                 110, hwnd, reinterpret_cast<HMENU>(kIdMods), inst, nullptr);
    CreateWindowExW(0, L"STATIC", L"Controls - Player 1:", WS_CHILD | WS_VISIBLE, 12, 348, 140, 18,
                    hwnd, nullptr, inst, nullptr);
    state.devices = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, 150, 344,
                                    280, 200, hwnd, reinterpret_cast<HMENU>(kIdDevices), inst,
                                    nullptr);
    CreateWindowExW(0, L"BUTTON", L"Rescan", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 440, 344, 80,
                    26, hwnd, reinterpret_cast<HMENU>(kIdRescanPads), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Use controller", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 344,
                    172, 26, hwnd, reinterpret_cast<HMENU>(kIdApplyPad), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Use keyboard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 376,
                    172, 26, hwnd, reinterpret_cast<HMENU>(kIdKeyboard), inst, nullptr);
    CreateWindowExW(0, L"STATIC",
                    L"Pick an entry and press Rebind (or double-click), then press the input you want:",
                    WS_CHILD | WS_VISIBLE, 12, 410, 560, 18, hwnd, nullptr, inst, nullptr);
    state.binds = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 10, 430, 510,
                                  190, hwnd, reinterpret_cast<HMENU>(kIdBindList), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Rebind", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 430, 172,
                    28, hwnd, reinterpret_cast<HMENU>(kIdBindOne), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 464, 172,
                    28, hwnd, reinterpret_cast<HMENU>(kIdClearOne), inst, nullptr);
    // Shown only when they apply; see DevRefreshJoyconButtons.
    state.combine = CreateWindowExW(0, L"BUTTON", L"Combine Joy-Cons into one player",
                                    WS_CHILD | BS_PUSHBUTTON | BS_MULTILINE, 528, 508, 172, 40,
                                    hwnd, reinterpret_cast<HMENU>(kIdCombineJoycons), inst,
                                    nullptr);
    state.split = CreateWindowExW(0, L"BUTTON", L"Split Joy-Cons into two players",
                                  WS_CHILD | BS_PUSHBUTTON | BS_MULTILINE, 528, 556, 172, 40, hwnd,
                                  reinterpret_cast<HMENU>(kIdSplitJoycons), inst, nullptr);
    CreateWindowExW(0, L"STATIC", L"Resolution Scale (takes full effect next launch):",
                    WS_CHILD | WS_VISIBLE, 12, 644, 320, 18, hwnd, nullptr, inst, nullptr);
    state.res_scale =
        CreateWindowExW(0, L"COMBOBOX", nullptr,
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, 340, 640, 360, 300,
                        hwnd, reinterpret_cast<HMENU>(kIdResScale), inst, nullptr);
    for (const auto& [setup, label] : kResScaleEntries) {
        SendMessageW(state.res_scale, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    const auto button = [&](const wchar_t* text, int x, int id) {
        CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, 680, 160, 28,
                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    button(L"Open user data folder", 10, kIdOpenUser);
    button(L"Open mods folder", 180, kIdOpenMods);
    button(L"Open keys folder", 350, kIdOpenKeys);
    button(L"Resume", 540, kIdClose);

    const HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM f) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(f), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font));

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    SetWindowTextW(state.status, DevStatusText(system).c_str());
    DevRefreshMods(state.mods);
    DevRefreshDevices(state);
    DevRefreshBinds(state);
    DevRefreshJoyconButtons(state);
    DevRefreshResScale(state);
    SetTimer(hwnd, kTimer, 500, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // Modal to the game: emulation stays paused-ish while the panel is up, and
    // the panel gets its own pump so the live status keeps refreshing.
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (previous_dpi != nullptr) {
        SetThreadDpiAwarenessContext(previous_dpi);
    }
}

} // namespace
#endif

EmuWindow_SDL2::EmuWindow_SDL2(InputCommon::InputSubsystem* input_subsystem_, Core::System& system_)
    : input_subsystem{input_subsystem_}, system{system_} {
    input_subsystem->Initialize();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        LOG_CRITICAL(Frontend, "Failed to initialize SDL3: {}, Exiting...", SDL_GetError());
        exit(1);
    }
}

EmuWindow_SDL2::~EmuWindow_SDL2() {
    // An assignment made in the last couple of seconds is still waiting to be written.
    if (controls_save_pending) {
        SaveNativeControls();
    }
    system.HIDCore().UnloadInputDevices();
    input_subsystem->Shutdown();
    SDL_Quit();
}

InputCommon::MouseButton EmuWindow_SDL2::SDLButtonToMouseButton(u32 button) const {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return InputCommon::MouseButton::Left;
    case SDL_BUTTON_RIGHT:
        return InputCommon::MouseButton::Right;
    case SDL_BUTTON_MIDDLE:
        return InputCommon::MouseButton::Wheel;
    case SDL_BUTTON_X1:
        return InputCommon::MouseButton::Backward;
    case SDL_BUTTON_X2:
        return InputCommon::MouseButton::Forward;
    default:
        return InputCommon::MouseButton::Undefined;
    }
}

std::pair<float, float> EmuWindow_SDL2::MouseToTouchPos(s32 touch_x, s32 touch_y) const {
    int w, h;
    SDL_GetWindowSize(render_window, &w, &h);
    const float fx = static_cast<float>(touch_x) / w;
    const float fy = static_cast<float>(touch_y) / h;

    return {std::clamp<float>(fx, 0.0f, 1.0f), std::clamp<float>(fy, 0.0f, 1.0f)};
}

void EmuWindow_SDL2::OnMouseButton(u32 button, u8 state, s32 x, s32 y) {
    const auto mouse_button = SDLButtonToMouseButton(button);
    if (state == SDL_PRESSED) {
        const auto [touch_x, touch_y] = MouseToTouchPos(x, y);
        input_subsystem->GetMouse()->PressButton(x, y, mouse_button);
        input_subsystem->GetMouse()->PressMouseButton(mouse_button);
        input_subsystem->GetMouse()->PressTouchButton(touch_x, touch_y, mouse_button);
    } else {
        input_subsystem->GetMouse()->ReleaseButton(mouse_button);
    }
}

void EmuWindow_SDL2::OnMouseMotion(s32 x, s32 y) {
    const auto [touch_x, touch_y] = MouseToTouchPos(x, y);
    input_subsystem->GetMouse()->Move(x, y, 0, 0);
    input_subsystem->GetMouse()->MouseMove(touch_x, touch_y);
    input_subsystem->GetMouse()->TouchMove(touch_x, touch_y);
}

void EmuWindow_SDL2::OnFingerDown(float x, float y, std::size_t id) {
    input_subsystem->GetTouchScreen()->TouchPressed(x, y, id);
}

void EmuWindow_SDL2::OnFingerMotion(float x, float y, std::size_t id) {
    input_subsystem->GetTouchScreen()->TouchMoved(x, y, id);
}

void EmuWindow_SDL2::OnFingerUp() {
    input_subsystem->GetTouchScreen()->ReleaseAllTouch();
}

void EmuWindow_SDL2::OnKeyEvent(int key, u8 state) {
#ifdef _WIN32
    if (state == SDL_PRESSED && key == SDL_SCANCODE_F12) {
        ShowDevMenu(system, input_subsystem);
        return;
    }
#endif
    if (state == SDL_PRESSED) {
        input_subsystem->GetKeyboard()->PressKey(static_cast<std::size_t>(key));
    } else if (state == SDL_RELEASED) {
        input_subsystem->GetKeyboard()->ReleaseKey(static_cast<std::size_t>(key));
    }
}

bool EmuWindow_SDL2::IsOpen() const {
    return is_open;
}

void EmuWindow_SDL2::EnableTasPlayback() {
    tas_playback = true;
}

void EmuWindow_SDL2::OnFrameDisplayed() {
    if (!tas_playback) {
        return;
    }
    // Called on the render thread once per presented frame. The TAS driver
    // consumes exactly one command per call, which is what keeps a recorded
    // script deterministic against the frames the guest actually renders.
    auto* const tas = input_subsystem->GetTas();
    tas->UpdateThread();

    const auto [state, progress, lengths] = tas->GetStatus();
    if (!tas_started) {
        // Start on the first displayed frame rather than at load: before the
        // guest presents anything there is no frame for command zero to land
        // on. Reset reloads the script so playback always begins at the top.
        tas->Reset();
        tas->StartStop();
        tas_started = true;
        LOG_INFO(Frontend, "TAS playback started, {} frames queued", lengths[0]);
        return;
    }
    if (state != InputCommon::TasInput::TasState::Stopped) {
        tas_progress = progress;
        return;
    }
    // Playback ran off the end of the script and tas_loop is off. Quitting here
    // is what makes --tas usable unattended; without it the replay finishes and
    // the process sits idle until something kills it.
    LOG_INFO(Frontend, "TAS playback finished, {} of {} frames, exiting", tas_progress,
             lengths[0]);
    tas_playback = false;
    // WaitEvent is blocked in SDL_WaitEvent on the main thread. SDL_PushEvent is
    // thread safe, and the main thread turns SDL_EVENT_QUIT into is_open = false,
    // so the shutdown stays on the thread that owns the window.
    SDL_Event quit_event{};
    quit_event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit_event);
}

bool EmuWindow_SDL2::IsShown() const {
    return is_shown;
}

void EmuWindow_SDL2::OnResize() {
    int width, height;
    SDL_GetWindowSizeInPixels(render_window, &width, &height);
    // A minimized window reports 0x0. Feeding that through as a layout makes
    // the renderer build a zero-extent swapchain, which the driver never
    // presents from - the window comes back blank and the main loop stops
    // answering. Keep the last good layout instead; the next real resize (or
    // the restore) delivers correct dimensions.
    if (width <= 0 || height <= 0) {
        return;
    }
    UpdateCurrentFramebufferLayout(width, height);
}

void EmuWindow_SDL2::ShowCursor(bool show_cursor) {
    if (show_cursor) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

void EmuWindow_SDL2::Fullscreen() {
    switch (Settings::values.fullscreen_mode.GetValue()) {
    case Settings::FullscreenMode::Exclusive:
        // Set window size to render size before entering fullscreen -- SDL3 does not resize window
        // to display dimensions automatically in this mode.
        {
            const SDL_DisplayMode* display_mode =
                SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
            if (display_mode) {
                SDL_SetWindowSize(render_window, display_mode->w, display_mode->h);
            } else {
                LOG_ERROR(Frontend, "SDL_GetDesktopDisplayMode failed: {}", SDL_GetError());
            }
        }

        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }

        LOG_ERROR(Frontend, "Fullscreening failed: {}", SDL_GetError());
        LOG_INFO(Frontend, "Attempting to use borderless fullscreen...");
        [[fallthrough]];
    case Settings::FullscreenMode::Borderless:
        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }

        LOG_ERROR(Frontend, "Borderless fullscreening failed: {}", SDL_GetError());
        [[fallthrough]];
    default:
        // Fallback algorithm: Maximise window.
        // Works on all systems (unless something is seriously wrong), so no fallback for this one.
        LOG_INFO(Frontend, "Falling back on a maximised window...");
        SDL_MaximizeWindow(render_window);
        break;
    }
}

void EmuWindow_SDL2::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    // Wait with a deadline rather than forever: with no input there is no event
    // to wake on, so a plain SDL_WaitEvent would never refresh the status.
    // A timeout is the idle tick, not a failure.
    SDL_ClearError();
    if (!SDL_WaitEventTimeout(&event, kEventWaitSliceMs)) {
        if (const char* error = SDL_GetError(); error != nullptr && error[0] != '\0') {
            LOG_ERROR(Frontend, "SDL_WaitEventTimeout failed: {}", error);
            SDL_ClearError();
        }
        const u64 idle_time = SDL_GetTicks();
        if (idle_time > last_time + kStatusRefreshMs) {
            last_time = idle_time;
            RefreshWindowStatus();
        }
        return;
    }


    switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        // Restoring only ever raised RESTORED, never EXPOSED, so is_shown was
        // left false from the minimize and the renderer stayed parked - the
        // window came back black and eventually stopped responding.
        is_shown = true;
        OnResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        is_shown = false;
        OnResize();
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = true;
        OnResize();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        is_open = false;
        break;
    case SDL_EVENT_KEY_DOWN:
        OnKeyEvent(static_cast<int>(event.key.scancode), SDL_PRESSED);
        break;
    case SDL_EVENT_KEY_UP:
        OnKeyEvent(static_cast<int>(event.key.scancode), SDL_RELEASED);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        // ignore if it came from touch
        if (event.motion.which != SDL_TOUCH_MOUSEID)
            OnMouseMotion(static_cast<s32>(event.motion.x), static_cast<s32>(event.motion.y));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        // ignore if it came from touch
        if (event.button.which != SDL_TOUCH_MOUSEID) {
            OnMouseButton(event.button.button,
                          event.button.down ? SDL_PRESSED : SDL_RELEASED,
                          static_cast<s32>(event.button.x), static_cast<s32>(event.button.y));
        }
        break;
    case SDL_EVENT_FINGER_DOWN:
        OnFingerDown(event.tfinger.x, event.tfinger.y,
                     static_cast<std::size_t>(event.tfinger.fingerID));
        break;
    case SDL_EVENT_FINGER_MOTION:
        OnFingerMotion(event.tfinger.x, event.tfinger.y,
                       static_cast<std::size_t>(event.tfinger.fingerID));
        break;
    case SDL_EVENT_FINGER_UP:
        OnFingerUp();
        break;
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        gamepad_check_pending = true;
        gamepad_retries = kGamepadRetries;
        break;
    default:
        break;
    }

    const u64 current_time = SDL_GetTicks();
    if (current_time > last_time + kStatusRefreshMs) {
        last_time = current_time;
        RefreshWindowStatus();
    }
}

#ifdef _WIN32
namespace {
// What the progress window paints. Written and read on the main thread only.
std::size_t g_build_progress_built = 0;
std::size_t g_build_progress_total = 0;

LRESULT CALLBACK BuildProgressProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg != WM_PAINT) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

    wchar_t text[96];
    if (g_build_progress_total == 0) {
        std::swprintf(text, std::size(text), L"Preparing shaders...");
    } else {
        std::swprintf(text, std::size(text), L"Preparing shaders: %zu of %zu",
                      g_build_progress_built, g_build_progress_total);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    RECT text_rect{16, 12, client.right - 16, 34};
    DrawTextW(dc, text, -1, &text_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT bar{16, 42, client.right - 16, 62};
    FrameRect(dc, &bar, GetSysColorBrush(COLOR_BTNSHADOW));
    if (g_build_progress_total != 0) {
        RECT fill = bar;
        InflateRect(&fill, -2, -2);
        const auto done = std::min(g_build_progress_built, g_build_progress_total);
        fill.right = fill.left + static_cast<LONG>((fill.right - fill.left) * done /
                                                   g_build_progress_total);
        FillRect(dc, &fill, GetSysColorBrush(COLOR_HIGHLIGHT));
    }
    EndPaint(hwnd, &ps);
    return 0;
}
} // namespace
#endif

void EmuWindow_SDL2::ShowBuildProgress(std::size_t built, std::size_t total) {
    // Throttle the title; thousands of pipelines shouldn't mean thousands of title updates.
    const u64 now = SDL_GetTicks();
    if (built == 0 || built == total || now >= last_build_title_ticks + 100) {
        last_build_title_ticks = now;
        char title[64];
        if (total == 0) {
            std::snprintf(title, sizeof(title), "Preparing shaders...");
        } else {
            std::snprintf(title, sizeof(title), "Building shaders %zu/%zu", built, total);
        }
        SDL_SetWindowTitle(render_window, title);
    }
#ifdef _WIN32
    g_build_progress_built = built;
    g_build_progress_total = total;
    if (!build_progress_window) {
        static const wchar_t* const kClass = L"SuyuBuildProgress";
        static const bool registered = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = BuildProgressProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_WAIT);
            wc.lpszClassName = kClass;
            return RegisterClassW(&wc) != 0;
        }();
        if (!registered) {
            return;
        }
        const auto owner = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(render_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        RECT owner_rect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (owner) {
            GetWindowRect(owner, &owner_rect);
        }
        constexpr int width = 420;
        constexpr int height = 78;
        const int x = owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2;
        const int y = owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2;
        // Owned by the game window, so it stays above it and closes with it.
        build_progress_window =
            CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP | WS_BORDER | WS_VISIBLE,
                            x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr),
                            nullptr);
    }
    if (build_progress_window) {
        InvalidateRect(static_cast<HWND>(build_progress_window), nullptr, FALSE);
        UpdateWindow(static_cast<HWND>(build_progress_window));
    }
#endif
}

void EmuWindow_SDL2::HideBuildProgress() {
#ifdef _WIN32
    if (build_progress_window) {
        DestroyWindow(static_cast<HWND>(build_progress_window));
        build_progress_window = nullptr;
    }
#endif
}

bool EmuWindow_SDL2::PumpEventsWhileLoading() {
    SDL_PumpEvents();
    // Leave the events queued: the main loop handles a close request as usual.
    return !SDL_HasEvent(SDL_EVENT_QUIT) && !SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
}

void EmuWindow_SDL2::RefreshWindowStatus() {
    // Same tick as the title: cheap, and a controller plugged in mid-game is
    // picked up within a second.
    if (gamepad_check_pending) {
        AutoAssignControllers();
    }
    if (controls_save_pending && SDL_GetTicks() >= last_controls_save + kControlsSaveMs) {
        controls_save_pending = false;
        last_controls_save = SDL_GetTicks();
        SaveNativeControls();
    }

    const bool benchmark_owns_stats = std::getenv("SUYU_CMD_PERF_SAMPLE") != nullptr;
    const auto snap = SuyuCmd::SampleNativeStatus(system, !benchmark_owns_stats);
    SuyuCmd::StoreNativeStatusSnapshot(snap);

    SuyuCmd::NativeStatusSnapshot display{};
    SuyuCmd::TryGetNativeStatusSnapshot(display);
    if (display.game_running) {
        display.shaders_building = system.GPU().ShaderNotify().ShadersBuilding();
    }
    const std::string title = SuyuCmd::FormatNativeTitle(display);
    SDL_SetWindowTitle(render_window, title.c_str());
}

// An export has no input settings screen in front of it, so a player who plugs
// in controllers expects them to just work: see SuyuCmd::AssignControllers for
// which pad gets which player. Only a standalone export does this - a plain
// suyu-cmd run keeps the player's own configuration - and controls picked in
// the F12 panel switch it off for good (auto_assign_controllers).
void EmuWindow_SDL2::AutoAssignControllers() {
    gamepad_check_pending = false;
    // A TAS replay drives the players itself; their mappings stay as configured.
    if (!g_export_package || !SdlConfig::auto_assign_controllers ||
        Settings::values.tas_enable.GetValue()) {
        return;
    }
    const auto devices = input_subsystem->GetInputDevices();
    std::size_t sdl_gamepads = 0;
    const auto pads = ControllerPads(devices, &sdl_gamepads);
    const auto sdl_listed = static_cast<std::size_t>(
        std::count_if(pads.begin(), pads.end(),
                      [](const auto& pad) { return pad.Get("engine", "") == "sdl"; }));
    // SDL can list a gamepad a moment before the input backend registers it,
    // so look again for a few ticks. Only a few: gamepads the backend leaves to
    // another driver (Joy-Cons under suyu's own driver) never show up here.
    if (gamepad_retries > 0 && sdl_listed < sdl_gamepads) {
        --gamepad_retries;
        gamepad_check_pending = true;
    }

    auto bindings = SuyuCmd::ParsePadBindings(SdlConfig::auto_assigned_pads);
    // GetValue() hands back a reference to the live array, so the mappings are
    // written straight into the setting.
    auto& players = Settings::values.players.GetValue();
    const auto map_pad = [this](const Common::ParamPackage& pad, Settings::PlayerInput& player) {
        MapDefault(*input_subsystem, pad, player);
        LOG_INFO(Frontend, "Assigned {}", pad.Get("display", std::string{"a controller"}));
    };
    // The layout SdlConfig gives a player with nothing configured.
    const auto restore_keyboard = [](Settings::PlayerInput& player) {
        for (std::size_t i = 0; i < player.buttons.size(); ++i) {
            player.buttons[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_buttons[i]);
        }
        for (std::size_t i = 0; i < player.analogs.size(); ++i) {
            const auto& keys = SdlConfig::default_analogs[i];
            player.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
                keys[0], keys[1], keys[2], keys[3], SdlConfig::default_stick_mod[i], 0.5f);
        }
        for (std::size_t i = 0; i < player.motions.size(); ++i) {
            player.motions[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_motions[i]);
        }
        LOG_INFO(Frontend, "No controller left: Player 1 uses the keyboard");
    };
    if (!SuyuCmd::AssignControllers(players, devices, pads, bindings, pads_seen,
                                    SdlConfig::default_buttons, map_pad, restore_keyboard)) {
        return;
    }
    SdlConfig::auto_assigned_pads = SuyuCmd::SerializePadBindings(bindings);
    for (const auto& binding : bindings) {
        LOG_INFO(Frontend, "Player {}: pad {} port {}, {}", binding.player + 1, binding.guid,
                 binding.port, players[binding.player].connected ? "connected" : "disconnected");
    }
    // The game sees the change now; the file is written at most every couple
    // of seconds, so a pad that keeps dropping out does not rewrite it each time.
    system.HIDCore().ReloadInputDevices();
    controls_save_pending = true;
}

// Credits to Samantas5855 and others for this function.
void EmuWindow_SDL2::SetWindowIcon() {
#ifdef _WIN32
    // Native game exports embed the ROM's own icon into the exe's PE resources
    // (RT_GROUP_ICON id 1, see suyu/game_export.cpp) — use that instead of the
    // suyu logo so the window reads as the game, not the emulator.
    if (g_native_export_mode) {
        const HICON hicon = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256,
                       LR_DEFAULTCOLOR));
        if (hicon != nullptr) {
            ICONINFO info{};
            if (GetIconInfo(hicon, &info)) {
                BITMAP bmp{};
                GetObjectW(info.hbmColor, sizeof(bmp), &bmp);
                const int w = bmp.bmWidth;
                const int h = bmp.bmHeight;
                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h; // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                const HDC hdc = GetDC(nullptr);
                if (GetDIBits(hdc, info.hbmColor, 0, h, pixels.data(), &bi, DIB_RGB_COLORS)) {
                    // BGRA -> RGBA
                    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                        std::swap(pixels[i], pixels[i + 2]);
                    }
                    SDL_Surface* const icon_surface = SDL_CreateSurfaceFrom(
                        w, h, SDL_PIXELFORMAT_RGBA32, pixels.data(), w * 4);
                    if (icon_surface != nullptr) {
                        SDL_SetWindowIcon(render_window, icon_surface);
                        SDL_DestroySurface(icon_surface);
                        ReleaseDC(nullptr, hdc);
                        DeleteObject(info.hbmColor);
                        DeleteObject(info.hbmMask);
                        DestroyIcon(hicon);
                        return;
                    }
                }
                ReleaseDC(nullptr, hdc);
                DeleteObject(info.hbmColor);
                DeleteObject(info.hbmMask);
            }
            DestroyIcon(hicon);
        }
        LOG_WARNING(Frontend, "Native export: failed to load game icon from exe resources, "
                               "falling back to suyu icon.");
    }
#endif
    SDL_IOStream* const suyu_icon_stream = SDL_IOFromConstMem((void*)suyu_icon, suyu_icon_size);
    if (suyu_icon_stream == nullptr) {
        LOG_WARNING(Frontend, "Failed to create suyu icon stream.");
        return;
    }
    SDL_Surface* const window_icon = SDL_LoadBMP_IO(suyu_icon_stream, true);
    if (window_icon == nullptr) {
        LOG_WARNING(Frontend, "Failed to read BMP from stream.");
        return;
    }
    // The icon is attached to the window pointer
    SDL_SetWindowIcon(render_window, window_icon);
    SDL_DestroySurface(window_icon);
}

void EmuWindow_SDL2::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    SDL_SetWindowMinimumSize(render_window, minimal_size.first, minimal_size.second);
}
