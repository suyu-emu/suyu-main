// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/bcat/backend/backend.h"
#include "core/hle/service/bcat/bcat.h"
#include "core/hle/service/bcat/news/news_service.h"
#include "core/hle/service/bcat/news/service_creator.h"
#include "core/hle/service/bcat/service_creator.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/set/settings_types.h"
#include "core/hle/service/set/system_settings_server.h"

namespace Service::BCAT {

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("bcat:a",
                                         std::make_shared<IServiceCreator>(system, "bcat:a"));
    server_manager->RegisterNamedService("bcat:m",
                                         std::make_shared<IServiceCreator>(system, "bcat:m"));
    server_manager->RegisterNamedService("bcat:u",
                                         std::make_shared<IServiceCreator>(system, "bcat:u"));
    server_manager->RegisterNamedService("bcat:s",
                                         std::make_shared<IServiceCreator>(system, "bcat:s"));

    Set::FirmwareVersionFormat firmware_version{};
    const auto firmware_result = Set::GetFirmwareVersionImpl(firmware_version, system,
                                                             Set::GetFirmwareVersionType::Version2);
    const bool use_legacy_news_service = firmware_result.IsSuccess() && firmware_version.major == 1;

    const auto make_news_service = [&](u32 permissions,
                                       const char* name) -> std::shared_ptr<SessionRequestHandler> {
        if (use_legacy_news_service) {
            return std::make_shared<News::INewsService>(system);
        }
        return std::make_shared<News::IServiceCreator>(system, permissions, name);
    };

    server_manager->RegisterNamedService("news:a", make_news_service(0xffffffff, "news:a"));
    server_manager->RegisterNamedService("news:p", make_news_service(0x1, "news:p"));
    server_manager->RegisterNamedService("news:c", make_news_service(0x2, "news:c"));
    server_manager->RegisterNamedService("news:v", make_news_service(0x4, "news:v"));
    server_manager->RegisterNamedService("news:m", make_news_service(0xd, "news:m"));

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::BCAT
