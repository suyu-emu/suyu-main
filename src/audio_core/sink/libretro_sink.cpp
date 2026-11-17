// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/sink/libretro_sink.h"

namespace AudioCore::Sink {

LibretroSink*& LibretroSink::Instance() {
    static LibretroSink* instance = nullptr;
    return instance;
}

std::mutex& LibretroSink::InstanceMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace AudioCore::Sink
