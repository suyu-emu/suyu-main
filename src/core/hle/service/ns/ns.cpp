// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/ns/application_manager_interface.h"
#include "core/hle/service/ns/develop_interface.h"
#include "core/hle/service/ns/ns.h"
#include "core/hle/service/ns/platform_service_manager.h"
#include "core/hle/service/ns/query_service.h"
#include "core/hle/service/ns/service_getter_interface.h"
#include "core/hle/service/ns/system_update_interface.h"
#include "core/hle/service/ns/vulnerability_manager_interface.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/set/settings_types.h"
#include "core/hle/service/set/system_settings_server.h"

namespace Service::NS {

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    // Before 3.0.0, ns:am exposed IApplicationManagerInterface directly. Later firmware uses
    // ns:am2 as an IServiceGetterInterface and obtains the application manager through command
    // 7996.
    Set::FirmwareVersionFormat firmware_version{};
    const auto firmware_result = Set::GetFirmwareVersionImpl(firmware_version, system,
                                                             Set::GetFirmwareVersionType::Version2);
    if (firmware_result.IsSuccess() && firmware_version.major < 3) {
        server_manager->RegisterNamedService(
            "ns:am", std::make_shared<IApplicationManagerInterface>(system));
    }
    server_manager->RegisterNamedService(
        "ns:am2", std::make_shared<IServiceGetterInterface>(system, "ns:am2"));
    server_manager->RegisterNamedService(
        "ns:ec", std::make_shared<IServiceGetterInterface>(system, "ns:ec"));
    server_manager->RegisterNamedService(
        "ns:rid", std::make_shared<IServiceGetterInterface>(system, "ns:rid"));
    server_manager->RegisterNamedService(
        "ns:rt", std::make_shared<IServiceGetterInterface>(system, "ns:rt"));
    server_manager->RegisterNamedService(
        "ns:web", std::make_shared<IServiceGetterInterface>(system, "ns:web"));
    server_manager->RegisterNamedService(
        "ns:ro", std::make_shared<IServiceGetterInterface>(system, "ns:ro"));

    server_manager->RegisterNamedService("ns:dev", std::make_shared<IDevelopInterface>(system));
    server_manager->RegisterNamedService("ns:su", std::make_shared<ISystemUpdateInterface>(system));
    server_manager->RegisterNamedService("ns:vm",
                                         std::make_shared<IVulnerabilityManagerInterface>(system));
    server_manager->RegisterNamedService("pdm:qry", std::make_shared<IQueryService>(system));

    server_manager->RegisterNamedService("pl:s",
                                         std::make_shared<IPlatformServiceManager>(system, "pl:s"));
    server_manager->RegisterNamedService("pl:u",
                                         std::make_shared<IPlatformServiceManager>(system, "pl:u"));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NS
