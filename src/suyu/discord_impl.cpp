// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QThread>

#include <discord_rpc.h>

#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "network/room_member.h"
#include "network/network.h"
#include "suyu/discord_impl.h"
#include "suyu/uisettings.h"
#include "suyu/wikipedia_cover.h"

namespace DiscordRPC {

namespace {

constexpr qint64 kCoverLookupBudgetMs = 6000;

// Session-wide state, touched only on the GUI thread. The cache holds a finished lookup per
// title, including an empty URL when nothing was found, so each title is looked up once.
std::map<std::string, std::string>& CoverCache() {
    static std::map<std::string, std::string> cache;
    return cache;
}

// Lookups still running. They are waited for when the application quits, so none is left
// using the network stack while it is torn down; each is bounded by kCoverLookupBudgetMs.
std::set<QThread*>& RunningLookups() {
    static std::set<QThread*> running;
    return running;
}

} // namespace

DiscordImpl::DiscordImpl(Core::System& system_)
    : lookup_receiver{std::make_unique<QObject>()}, system{system_} {
    DiscordEventHandlers handlers{};
    // The number is the client ID for suyu, it's used for images and the
    // application name
    // NOTE: This application is owned by million1156 (million@alyocord.com)
    Discord_Initialize("1221314350216646828", &handlers, 1, nullptr);
}

DiscordImpl::~DiscordImpl() {
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    Discord_ClearPresence();
}

void DiscordImpl::UpdateGameStatus() {
    const std::string default_text = "suyu is an emulator for the Nintendo Switch";
    const std::string default_image = "suyu_logo";
    const std::string& url = game_url.empty() ? default_image : game_url;
    DiscordRichPresence presence{};

    std::string room_state;
    if (const auto member = system.GetRoomNetwork().GetRoomMember().lock();
        member && member->IsConnected()) {
        const auto room_name = member->GetRoomInformation().name;
        room_state = room_name.empty() ? "In a NetPlay room" : "NetPlay: " + room_name;
        // Discord limits activity strings to 128 bytes. Leave room for the
        // prefix and avoid publishing the room's address or password.
        if (room_state.size() > 128) {
            size_t length = 128;
            while (length > 0 &&
                   (static_cast<unsigned char>(room_state[length]) & 0xC0) == 0x80) {
                --length;
            }
            room_state.resize(length);
        }
    }

    presence.largeImageKey = url.c_str();
    presence.largeImageText = game_title.c_str();
    presence.smallImageKey = default_image.c_str();
    presence.smallImageText = default_text.c_str();
    presence.state = room_state.empty() ? game_title.c_str() : room_state.c_str();
    presence.details = "Currently in game";
    presence.startTimestamp = game_start_timestamp;
    Discord_UpdatePresence(&presence);
}

void DiscordImpl::LookUpCover() {
    if (const auto cached = CoverCache().find(game_title); cached != CoverCache().end()) {
        game_url = cached->second;
        return;
    }
    static std::set<std::string> in_flight;
    if (!in_flight.insert(game_title).second) {
        return;
    }
    static bool waits_on_quit = false;
    if (!waits_on_quit && QCoreApplication::instance()) {
        waits_on_quit = true;
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [] {
            for (QThread* thread : RunningLookups()) {
                thread->wait(kCoverLookupBudgetMs + 1000);
            }
        });
    }

    // The lookup blocks in its own event loop, so it runs on a worker thread; the GUI thread
    // only starts it and later receives the URL.
    const std::string title = game_title;
    const auto result = std::make_shared<std::string>();
    QThread* thread = QThread::create([title, result] {
        QNetworkAccessManager network;
        QElapsedTimer clock;
        clock.start();
        *result = WikipediaCover::DiscordImageUrl(
                      WikipediaCover::FindCoverUrls(network, QString::fromStdString(title), clock,
                                                    kCoverLookupBudgetMs,
                                                    QStringLiteral("suyu (Discord cover art)")))
                      .toStdString();
    });
    RunningLookups().insert(thread);
    // Emitted on the worker thread; both receivers live on the GUI thread, so these run there.
    QObject::connect(thread, &QThread::finished, thread, [thread, title, result] {
        CoverCache()[title] = *result;
        in_flight.erase(title);
        RunningLookups().erase(thread);
        if (result->empty()) {
            LOG_INFO(Frontend, "Discord presence: no Wikipedia cover art found for \"{}\"",
                     title);
        }
        thread->deleteLater();
    });
    QObject::connect(thread, &QThread::finished, lookup_receiver.get(), [this, title, result] {
        if (result->empty() || !system.IsPoweredOn() || game_title != title) {
            return;
        }
        game_url = *result;
        LOG_INFO(Frontend, "Discord presence: updated \"{}\" with image key {}", title, game_url);
        UpdateGameStatus();
    });
    thread->start(QThread::LowPriority);
}

void DiscordImpl::Update() {
    const std::string default_text = "suyu is an emulator for the Nintendo Switch";
    const std::string default_image = "suyu_logo";

    if (system.IsPoweredOn()) {
        std::string loaded_title;
        system.GetAppLoader().ReadTitle(loaded_title);
        if (loaded_title.empty()) {
            loaded_title = "Nintendo Switch game";
        }
        if (loaded_title == game_title) {
            UpdateGameStatus();
            return;
        }
        game_title = std::move(loaded_title);
        game_url.clear();
        game_start_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();

        // Published right away with the cached cover or the suyu logo. An uncached cover is
        // looked up on Wikipedia, which receives the title, and replaces the logo when found.
        LookUpCover();
        LOG_INFO(Frontend, "Discord presence: publishing \"{}\" with image key {}", game_title,
                 game_url.empty() ? default_image : game_url);
        UpdateGameStatus();
        return;
    }

    game_title.clear();
    game_url.clear();
    game_start_timestamp = 0;
    s64 start_time = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    DiscordRichPresence presence{};
    presence.largeImageKey = default_image.c_str();
    presence.largeImageText = default_text.c_str();
    presence.details = "Currently not in game";
    presence.startTimestamp = start_time;
    Discord_UpdatePresence(&presence);
}
} // namespace DiscordRPC
