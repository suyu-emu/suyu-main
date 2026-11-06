// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/bcat/news/newly_arrived_event_holder.h"
#include "core/hle/service/bcat/news/news_data_service.h"
#include "core/hle/service/bcat/news/news_database_service.h"
#include "core/hle/service/bcat/news/news_service.h"
#include "core/hle/service/bcat/news/news_storage.h"
#include "core/hle/service/cmif_serialization.h"

#include <cstring>

namespace Service::News {

INewsService::INewsService(Core::System& system_) : ServiceFramework{system_, "INewsService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {10100, D<&INewsService::PostLocalNews>, "PostLocalNews"},
        {20100, D<&INewsService::SetPassphrase>, "SetPassphrase"},
        {30100, D<&INewsService::GetSubscriptionStatus>, "GetSubscriptionStatus"},
        {30101, D<&INewsService::GetTopicList>, "GetTopicList"}, //3.0.0+
        {30110, D<&INewsService::GetTopicList>, "Unknown30110"}, //6.0.0+ (stub)
        {30200, D<&INewsService::IsSystemUpdateRequired>, "IsSystemUpdateRequired"},
        {30201, D<&INewsService::IsSystemUpdateRequired>, "Unknown30201"}, //8.0.0+ (stub)
        {30210, D<&INewsService::IsSystemUpdateRequired>, "Unknown30210"}, //10.0.0+ (stub)
        {30300, nullptr, "RequestImmediateReception"},
        {30400, nullptr, "DecodeArchiveFile"}, //3.0.0-18.1.0 (stub)
        {30500, nullptr, "Unknown30500"}, //8.0.0+ (stub)
        {30900, D<&INewsService::CreateNewlyArrivedEventHolderOld>, "CreateNewlyArrivedEventHolderOld"}, //1.0.0
        {30901, D<&INewsService::CreateNewsDataServiceOld>, "CreateNewsDataServiceOld"}, //1.0.0
        {30902, D<&INewsService::CreateNewsDatabaseServiceOld>, "CreateNewsDatabaseServiceOld"}, //1.0.0
        {40100, nullptr, "SetSubscriptionStatus"},
        {40101, D<&INewsService::RequestAutoSubscription>, "RequestAutoSubscription"}, //3.0.0+
        {40200, D<&INewsService::ClearStorage>, "ClearStorage"},
        {40201, D<&INewsService::ClearSubscriptionStatusAll>, "ClearSubscriptionStatusAll"},
        {90100, D<&INewsService::GetNewsDatabaseDump>, "GetNewsDatabaseDump"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

INewsService::~INewsService() = default;

Result INewsService::PostLocalNews(InBuffer<BufferAttr_HipcAutoSelect> buffer_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) PostLocalNews size={}", buffer_data.size());
    R_SUCCEED();
}

Result INewsService::GetSubscriptionStatus(Out<u32> out_status,
                                           InBuffer<BufferAttr_HipcPointer> buffer_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called, buffer_size={}", buffer_data.size());
    *out_status = 0;
    R_SUCCEED();
}

Result INewsService::IsSystemUpdateRequired(Out<bool> out_is_system_update_required) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    *out_is_system_update_required = false;
    R_SUCCEED();
}

Result INewsService::CreateNewlyArrivedEventHolderOld(
    OutInterface<INewlyArrivedEventHolder> out_interface) {
    LOG_INFO(Service_BCAT, "called");
    *out_interface = std::make_shared<INewlyArrivedEventHolder>(system);
    R_SUCCEED();
}

Result INewsService::CreateNewsDataServiceOld(OutInterface<INewsDataService> out_interface) {
    LOG_INFO(Service_BCAT, "called");
    *out_interface = std::make_shared<INewsDataService>(system);
    R_SUCCEED();
}

Result INewsService::CreateNewsDatabaseServiceOld(
    OutInterface<INewsDatabaseService> out_interface) {
    LOG_INFO(Service_BCAT, "called");
    *out_interface = std::make_shared<INewsDatabaseService>(system);
    R_SUCCEED();
}

Result INewsService::RequestAutoSubscription(u64 value) {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::SetPassphrase(InBuffer<BufferAttr_HipcPointer> buffer_data) {
    LOG_WARNING(Service_BCAT, "(STUBBED) SetPassphrase called size={}", buffer_data.size());
    R_SUCCEED();
}

Result INewsService::GetTopicList(Out<s32> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_topics, s32 filter) {
    constexpr size_t TopicIdSize = 32;
    constexpr auto EdenTopicId = "suyu";

    const size_t max_topics = out_topics.size() / TopicIdSize;

    if (max_topics == 0) {
        *out_count = 0;
        R_SUCCEED();
    }

    std::memset(out_topics.data(), 0, out_topics.size());

    std::memcpy(out_topics.data(), EdenTopicId, std::strlen(EdenTopicId));
    *out_count = 1;

    R_SUCCEED();
}

Result INewsService::ClearStorage() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    NewsStorage::Instance().Clear();
    R_SUCCEED();
}

Result INewsService::ClearSubscriptionStatusAll() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

Result INewsService::GetNewsDatabaseDump() {
    LOG_WARNING(Service_BCAT, "(STUBBED) called");
    R_SUCCEED();
}

} // namespace Service::News
