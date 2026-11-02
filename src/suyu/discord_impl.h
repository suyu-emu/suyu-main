// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "suyu/discord.h"

class QObject;

namespace Core {
class System;
}

namespace DiscordRPC {

class DiscordImpl : public DiscordInterface {
public:
    DiscordImpl(Core::System& system_);
    ~DiscordImpl() override;

    void Pause() override;
    void Update() override;

private:
    void UpdateGameStatus();
    /// Starts a background Wikipedia lookup of the running game's cover art, unless one is
    /// cached or already running. The result arrives on the GUI thread.
    void LookUpCover();

    std::string game_url{};
    std::string game_title{};
    std::int64_t game_start_timestamp = 0;

    /// Lives on the GUI thread and receives lookup results; destroying it drops any that
    /// are still pending.
    std::unique_ptr<QObject> lookup_receiver;

    Core::System& system;
};

} // namespace DiscordRPC
