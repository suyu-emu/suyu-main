// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/param_package.h"
#include "common/settings_input.h"

namespace SuyuCmd {

/// A controller this export gave a player slot, remembered so the same controller returns to
/// the same slot after it is unplugged. The input backends number identical controllers by
/// port; two identical pads can swap slots if they are replugged in the other order, which is
/// harmless and how a console behaves too.
struct PadBinding {
    std::string guid;
    int port{};
    std::size_t player{};
};

/// "guid,port,player;..." as stored in the package config.
inline std::vector<PadBinding> ParsePadBindings(const std::string& text) {
    // Whole field, digits only: a hand-edited or damaged entry is dropped.
    const auto number = [](std::string_view field, int& out) {
        const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), out);
        return !field.empty() && error == std::errc{} && end == field.data() + field.size() &&
               out >= 0;
    };
    std::vector<PadBinding> bindings;
    std::stringstream entries(text);
    std::string entry;
    while (std::getline(entries, entry, ';')) {
        const auto first = entry.find(',');
        const auto second = first == std::string::npos ? first : entry.find(',', first + 1);
        if (first == 0 || second == std::string::npos) {
            continue;
        }
        const std::string_view view{entry};
        int port = 0;
        int player = 0;
        if (number(view.substr(first + 1, second - first - 1), port) &&
            number(view.substr(second + 1), player) && player < 8) {
            bindings.push_back({entry.substr(0, first), port, static_cast<std::size_t>(player)});
        }
    }
    return bindings;
}

inline std::string SerializePadBindings(const std::vector<PadBinding>& bindings) {
    std::string text;
    for (const auto& binding : bindings) {
        text += (text.empty() ? "" : ";") + binding.guid + "," + std::to_string(binding.port) +
                "," + std::to_string(binding.player);
    }
    return text;
}

using PadMapper = std::function<void(const Common::ParamPackage&, Settings::PlayerInput&)>;

namespace detail {

constexpr std::size_t kSlots = 8;

/// A Joy-Con pair listed as one device, next to its two halves.
inline bool IsPair(const Common::ParamPackage& pad) {
    return pad.Has("guid2");
}

/// Whether @p mapping belongs to either half of @p pair.
inline bool MapsHalfOf(const Common::ParamPackage& mapping, const Common::ParamPackage& pair) {
    const std::string guid = mapping.Get("guid", "");
    return mapping.Get("engine", "") == pair.Get("engine", "") &&
           mapping.Get("port", 0) == pair.Get("port", 0) &&
           (guid == pair.Get("guid", "") || guid == pair.Get("guid2", ""));
}

inline bool SameDevice(const Common::ParamPackage& a, const Common::ParamPackage& b) {
    return a.Get("engine", "") == b.Get("engine", "") && a.Get("guid", "") == b.Get("guid", "") &&
           a.Get("port", 0) == b.Get("port", 0);
}

/// What a controller plays as: a pair as Dual Joy-Cons; a single Joy-Con from suyu's own
/// driver as a sideways Joy-Con; anything else, including what SDL presents as a full
/// gamepad, as a Pro Controller.
inline Settings::ControllerType TypeFor(const Common::ParamPackage& pad) {
    if (IsPair(pad)) {
        return Settings::ControllerType::DualJoyconDetached;
    }
    if (pad.Get("engine", "") == "joycon") {
        switch (pad.Get("pad", 0)) {
        case 1:
            return Settings::ControllerType::LeftJoycon;
        case 2:
            return Settings::ControllerType::RightJoycon;
        default:
            break;
        }
    }
    return Settings::ControllerType::ProController;
}

inline void Clear(Settings::PlayerInput& player) {
    for (auto& button : player.buttons) {
        button.clear();
    }
    for (auto& analog : player.analogs) {
        analog.clear();
    }
    for (auto& motion : player.motions) {
        motion.clear();
    }
}

inline void Give(Settings::PlayerInput& player, const Common::ParamPackage& pad,
                 Settings::ControllerType type, const PadMapper& map_pad) {
    Clear(player);
    map_pad(pad, player);
    player.controller_type = type;
    player.connected = true;
}

/// Slot questions shared by the automatic and the F12 paths.
struct Slots {
    std::array<Settings::PlayerInput, 10>& players;
    const std::vector<Common::ParamPackage>& devices;
    const std::array<int, Settings::NativeButton::NumButtons>& stock_keys;

    Common::ParamPackage Mapping(std::size_t slot) const {
        return Common::ParamPackage{players[slot].buttons[Settings::NativeButton::A]};
    }
    bool IsStock(std::size_t slot) const {
        for (std::size_t i = 0; i < players[slot].buttons.size(); ++i) {
            const Common::ParamPackage key{players[slot].buttons[i]};
            if (key.Get("engine", "") != "keyboard" || key.Get("code", -1) != stock_keys[i]) {
                return false;
            }
        }
        return true;
    }
    /// Unmapped, the stock keyboard layout, or mapped to a device that is not connected.
    bool IsFree(std::size_t slot) const {
        const auto mapping = Mapping(slot);
        const std::string engine = mapping.Get("engine", "");
        if (engine.empty() || IsStock(slot)) {
            return true;
        }
        // The keyboard and mouse are always there.
        if (engine == "keyboard" || engine == "mouse") {
            return false;
        }
        return std::none_of(devices.begin(), devices.end(),
                            [&](const auto& device) { return SameDevice(device, mapping); });
    }
    /// Slots whose mapping is @p pad itself or, for a pair, either of its halves.
    std::vector<std::size_t> Holding(const Common::ParamPackage& pad) const {
        std::vector<std::size_t> held;
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            const auto mapping = Mapping(slot);
            if (SameDevice(mapping, pad) || (IsPair(pad) && MapsHalfOf(mapping, pad))) {
                held.push_back(slot);
            }
        }
        return held;
    }
};

} // namespace detail

/// Gives connected controllers to Players 1-8, the way a console hands them out.
///
/// - A new controller takes the slot it had before, if that slot is free, else the lowest
///   free slot; Player 1 whenever it is free. A slot is free when it is unmapped, still has
///   the stock keyboard layout, or is mapped to a device that is not connected; a mapping the
///   player made to a connected device is never taken.
/// - A controller unplugged during this session keeps its slot among Players 2-8 until it
///   returns, unless no other slot is free. Player 1 is not kept that way: when its
///   controller is unplugged while others stay in, Player 1 shows as disconnected (as on a
///   Switch) and the others stay where they are, but the next controller to connect, its own
///   or another, takes Player 1. Bindings saved by an earlier session only guide a returning
///   controller; they never hold a slot at startup.
/// - When a restart finds Player 1 without its controller but one of ours on a later player,
///   that one moves to Player 1.
/// - A player whose controller was unplugged is marked disconnected, so the game sees it
///   leave, and connected again on its return. When the last controller leaves and nothing
///   else is connected, Player 1 goes back to the stock keyboard layout and stays connected.
/// - Joy-Cons: a pair listed as one device plays as one player (Dual Joy-Cons); its halves
///   get no slots of their own. When the halves arrive one at a time, the slot the first half
///   took becomes the pair's and a slot the other half took is cleared - as long as both were
///   given out automatically; a split the player made is kept.
///
/// @p devices is every input device the backends list (used to tell which mappings are
/// live); @p pads are the controllers among them, in the backends' order. @p seen collects
/// the controllers connected at any point this session. @p map_pad writes a controller's
/// default mapping into a player and @p restore_keyboard the stock keyboard layout. Returns
/// true when players or bindings changed.
inline bool AssignControllers(
    std::array<Settings::PlayerInput, 10>& players, const std::vector<Common::ParamPackage>& devices,
    const std::vector<Common::ParamPackage>& pads, std::vector<PadBinding>& bindings,
    std::set<std::string>& seen,
    const std::array<int, Settings::NativeButton::NumButtons>& stock_keys, const PadMapper& map_pad,
    const std::function<void(Settings::PlayerInput&)>& restore_keyboard) {
    using namespace detail;
    const Slots slots{players, devices, stock_keys};
    const auto key_of = [](const std::string& guid, int port) {
        return guid + ":" + std::to_string(port);
    };
    const auto binding_matches = [](const PadBinding& binding, const Common::ParamPackage& pad) {
        return binding.guid == pad.Get("guid", "") && binding.port == pad.Get("port", 0);
    };
    const auto binding_connected = [&](const PadBinding& binding) {
        return std::any_of(pads.begin(), pads.end(),
                           [&](const auto& pad) { return binding_matches(binding, pad); });
    };
    // Any button, not just A: suyu's Joy-Con driver lists a pair under its left half's GUID
    // while A, like every face button, maps to the right half.
    const auto maps_binding = [&](std::size_t slot, const PadBinding& binding) {
        const auto& buttons = players[slot].buttons;
        return std::any_of(buttons.begin(), buttons.end(), [&](const std::string& button) {
            const Common::ParamPackage mapping{button};
            const std::string engine = mapping.Get("engine", "");
            return !engine.empty() && engine != "keyboard" && engine != "mouse" &&
                   mapping.Get("guid", "") == binding.guid &&
                   mapping.Get("port", 0) == binding.port;
        });
    };
    const auto ours = [&](std::size_t slot) {
        return std::any_of(bindings.begin(), bindings.end(),
                           [&](const auto& b) { return b.player == slot; });
    };
    // Held for a controller that was unplugged during this session.
    const auto reserved = [&](std::size_t slot, const Common::ParamPackage* pad) {
        return slot != 0 && std::any_of(bindings.begin(), bindings.end(), [&](const auto& b) {
                   return b.player == slot && (pad == nullptr || !binding_matches(b, *pad)) &&
                          !binding_connected(b) && seen.contains(key_of(b.guid, b.port));
               });
    };
    const auto forget_slot = [&](std::size_t slot) {
        Clear(players[slot]);
        players[slot].connected = false;
        std::erase_if(bindings, [&](const auto& b) { return b.player == slot; });
    };
    const auto give = [&](const Common::ParamPackage& pad, std::size_t slot) {
        Give(players[slot], pad, TypeFor(pad), map_pad);
        // One binding per controller and per slot: a slot handed to another controller
        // forgets the one that had it.
        std::erase_if(bindings, [&](const auto& b) {
            return b.player == slot || binding_matches(b, pad);
        });
        bindings.push_back({pad.Get("guid", ""), pad.Get("port", 0), slot});
    };

    // A Joy-Con pair is listed both whole and as its two halves.
    std::vector<const Common::ParamPackage*> usable;
    for (const auto& pad : pads) {
        const bool half_of_pair =
            !IsPair(pad) && std::any_of(pads.begin(), pads.end(), [&](const auto& pair) {
                return IsPair(pair) && MapsHalfOf(pad, pair);
            });
        if (!half_of_pair) {
            usable.push_back(&pad);
        }
    }

    bool changed = false;
    std::array<bool, kSlots> taken{};
    std::vector<const Common::ParamPackage*> unplaced;
    for (const auto* pad : usable) {
        const auto holding = slots.Holding(*pad);
        if (holding.empty()) {
            unplaced.push_back(pad);
            continue;
        }
        const std::size_t slot = holding.front();
        // The second half of a pair arrived: the pair takes the first half's slot, and a slot
        // the second half took on its own is cleared. Only when this export gave them out.
        if (IsPair(*pad) &&
            (players[slot].controller_type != Settings::ControllerType::DualJoyconDetached ||
             holding.size() > 1) &&
            std::all_of(holding.begin(), holding.end(), ours)) {
            for (std::size_t i = 1; i < holding.size(); ++i) {
                forget_slot(holding[i]);
            }
            give(*pad, slot);
            taken[slot] = true;
            changed = true;
            continue;
        }
        // One half of a pair left: the player they shared becomes the remaining half alone,
        // rather than Dual Joy-Cons with dead controls on one side.
        if (!IsPair(*pad) &&
            players[slot].controller_type == Settings::ControllerType::DualJoyconDetached &&
            ours(slot)) {
            give(*pad, slot);
            taken[slot] = true;
            changed = true;
            continue;
        }
        for (const std::size_t s : holding) {
            taken[s] = true;
        }
        // Back after being unplugged: only a slot this export filled is switched on again,
        // never one the player set up themselves.
        const bool returning = std::any_of(bindings.begin(), bindings.end(), [&](const auto& b) {
            return b.player == slot && binding_matches(b, *pad);
        });
        if (returning && !players[slot].connected) {
            players[slot].connected = true;
            changed = true;
        }
    }

    for (const auto* pad : unplaced) {
        const auto remembered = std::find_if(bindings.begin(), bindings.end(), [&](const auto& b) {
            return binding_matches(b, *pad);
        });
        std::size_t slot = kSlots;
        if (!taken[0] && slots.IsFree(0)) {
            slot = 0;
        } else if (remembered != bindings.end() && !taken[remembered->player] &&
                   slots.IsFree(remembered->player)) {
            slot = remembered->player;
        }
        for (std::size_t s = 1; s < kSlots && slot == kSlots; ++s) {
            if (!taken[s] && slots.IsFree(s) && !reserved(s, pad)) {
                slot = s;
            }
        }
        for (std::size_t s = 1; s < kSlots && slot == kSlots; ++s) {
            if (!taken[s] && slots.IsFree(s)) {
                slot = s;
            }
        }
        if (slot == kSlots) {
            continue;
        }
        give(*pad, slot);
        taken[slot] = true;
        changed = true;
    }

    // Player 1 still without a live controller while one of ours sits higher up (say only
    // Player 2's came back this time): it moves down to Player 1, unless Player 1's own
    // controller is only briefly unplugged.
    if (!taken[0] && slots.IsFree(0) &&
        !std::any_of(bindings.begin(), bindings.end(), [&](const auto& b) {
            return b.player == 0 && !binding_connected(b) && seen.contains(key_of(b.guid, b.port));
        })) {
        for (std::size_t s = 1; s < kSlots; ++s) {
            const auto held = std::find_if(bindings.begin(), bindings.end(), [&](const auto& b) {
                return b.player == s && maps_binding(s, b) && binding_connected(b);
            });
            if (!taken[s] || held == bindings.end()) {
                continue;
            }
            // Among the usable controllers: a Joy-Con pair shares its GUID with one half.
            const auto pad = std::find_if(usable.begin(), usable.end(),
                                          [&](const auto* p) { return binding_matches(*held, *p); });
            if (pad == usable.end()) {
                continue;
            }
            const Common::ParamPackage moving = **pad;
            forget_slot(s);
            give(moving, 0);
            taken[0] = true;
            taken[s] = false;
            changed = true;
            break;
        }
    }

    for (const auto& binding : bindings) {
        const std::size_t slot = binding.player;
        auto& player = players[slot];
        if (binding_connected(binding) || !maps_binding(slot, binding)) {
            continue;
        }
        if (slot == 0 && usable.empty()) {
            // Nobody left to hold a controller: Player 1 plays on the keyboard again. The
            // binding stays, so the controller takes Player 1 back on its return.
            restore_keyboard(player);
            player.controller_type = Settings::ControllerType::ProController;
            player.connected = true;
            changed = true;
        } else if (player.connected) {
            player.connected = false;
            changed = true;
        }
    }

    for (const auto* pad : usable) {
        seen.insert(key_of(pad->Get("guid", ""), pad->Get("port", 0)));
    }
    return changed;
}

/// F12 "Combine Joy-Cons": a connected pair whose halves are on two different players becomes
/// one player, Dual Joy-Cons, in the lower slot; the other slot is cleared and disconnected.
/// With @p map_pad empty nothing changes and only the slot it would use is returned.
inline std::optional<std::size_t> CombineJoycons(
    std::array<Settings::PlayerInput, 10>& players, const std::vector<Common::ParamPackage>& devices,
    const std::array<int, Settings::NativeButton::NumButtons>& stock_keys,
    const PadMapper& map_pad) {
    using namespace detail;
    const Slots slots{players, devices, stock_keys};
    for (const auto& pair : devices) {
        if (!IsPair(pair)) {
            continue;
        }
        const auto holding = slots.Holding(pair);
        if (holding.size() < 2) {
            continue;
        }
        if (map_pad) {
            for (std::size_t i = 1; i < holding.size(); ++i) {
                Clear(players[holding[i]]);
                players[holding[i]].connected = false;
            }
            Give(players[holding.front()], pair, Settings::ControllerType::DualJoyconDetached,
                 map_pad);
        }
        return holding.front();
    }
    return std::nullopt;
}

/// F12 "Split Joy-Cons": a pair on one player becomes two sideways Joy-Cons, the left half on
/// that player and the right half on the next free player after it, each with its own
/// default mapping. With @p map_pad empty nothing changes and only the pair's slot is
/// returned.
inline std::optional<std::size_t> SplitJoycons(
    std::array<Settings::PlayerInput, 10>& players, const std::vector<Common::ParamPackage>& devices,
    const std::array<int, Settings::NativeButton::NumButtons>& stock_keys,
    const PadMapper& map_pad) {
    using namespace detail;
    const Slots slots{players, devices, stock_keys};
    for (const auto& pair : devices) {
        if (!IsPair(pair)) {
            continue;
        }
        const auto holding = slots.Holding(pair);
        if (holding.size() != 1 || players[holding.front()].controller_type !=
                                       Settings::ControllerType::DualJoyconDetached) {
            continue;
        }
        // SDL lists a pair under its right half's GUID, suyu's own driver under its left.
        const bool left_first = pair.Get("engine", "") == "joycon";
        const std::string left_guid = pair.Get(left_first ? "guid" : "guid2", "");
        const std::string right_guid = pair.Get(left_first ? "guid2" : "guid", "");
        const auto half = [&](const std::string& guid) {
            return std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
                return !IsPair(device) && device.Get("engine", "") == pair.Get("engine", "") &&
                       device.Get("guid", "") == guid &&
                       device.Get("port", 0) == pair.Get("port", 0);
            });
        };
        const auto left = half(left_guid);
        const auto right = half(right_guid);
        const std::size_t slot = holding.front();
        std::size_t next = kSlots;
        for (std::size_t s = slot + 1; s < kSlots && next == kSlots; ++s) {
            if (slots.IsFree(s)) {
                next = s;
            }
        }
        if (left == devices.end() || right == devices.end() || next == kSlots) {
            continue;
        }
        if (map_pad) {
            Give(players[slot], *left, Settings::ControllerType::LeftJoycon, map_pad);
            Give(players[next], *right, Settings::ControllerType::RightJoycon, map_pad);
        }
        return slot;
    }
    return std::nullopt;
}

} // namespace SuyuCmd
