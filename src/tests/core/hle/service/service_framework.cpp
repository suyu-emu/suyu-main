// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/hle/service/service.h"

namespace Service {
namespace {

class RegistrationTestService final : public ServiceFramework<RegistrationTestService> {
public:
    explicit RegistrationTestService(Core::System& system_ref)
        : ServiceFramework{system_ref, "registration-test"} {
        static const FunctionInfo functions[] = {
            {0, &RegistrationTestService::Handle, "Cmif0"},
            {1, &RegistrationTestService::Handle, "Cmif1"},
            {2, &RegistrationTestService::Handle, "Cmif2"},
            {3, &RegistrationTestService::Handle, "Cmif3"},
            {4, &RegistrationTestService::Handle, "Cmif4"},
            {5, &RegistrationTestService::Handle, "Cmif5"},
        };
        static const FunctionInfo tipc_functions[] = {
            {0, &RegistrationTestService::Handle, "Tipc0"},
            {1, &RegistrationTestService::Handle, "Tipc1"},
            {2, &RegistrationTestService::Handle, "Tipc2"},
            {3, &RegistrationTestService::Handle, "Tipc3"},
            {4, &RegistrationTestService::Handle, "Tipc4"},
            {5, &RegistrationTestService::Handle, "Tipc5"},
        };
        RegisterHandlers(functions);
        RegisterHandlersTipc(tipc_functions);
    }

    [[nodiscard]] bool HasCmifHandler(u32 command) const {
        return handlers.find(command) != handlers.end();
    }

    [[nodiscard]] bool HasTipcHandler(u32 command) const {
        return handlers_tipc.find(command) != handlers_tipc.end();
    }

    [[nodiscard]] std::size_t CmifHandlerCount() const {
        return handlers.size();
    }

    [[nodiscard]] std::size_t TipcHandlerCount() const {
        return handlers_tipc.size();
    }

private:
    void Handle(HLERequestContext&) {}
};

} // namespace

TEST_CASE("ServiceFramework preserves every typed handler entry",
          "[core][hle][service][main-integration]") {
    Core::System system;
    RegistrationTestService service{system};

    REQUIRE(service.CmifHandlerCount() == 6);
    REQUIRE(service.TipcHandlerCount() == 6);
    for (u32 command = 0; command < 6; ++command) {
        CHECK(service.HasCmifHandler(command));
        CHECK(service.HasTipcHandler(command));
    }
}

} // namespace Service
