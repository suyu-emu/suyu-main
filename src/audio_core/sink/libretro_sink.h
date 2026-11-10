// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {

class LibretroSinkStreamImpl final : public SinkStream {
public:
    explicit LibretroSinkStreamImpl(Core::System& system_, StreamType type_, bool is_primary_,
                                    u32 system_channels_)
        : SinkStream{system_, type_}, is_primary{is_primary_} {
        system_channels = system_channels_;
        device_channels = 2;
    }
    ~LibretroSinkStreamImpl() override = default;

    void Start(bool resume = false) override {
        if (!detached) {
            paused = false;
        }
    }

    void Stop() override {
        SignalPause();
    }

    void AppendBuffer(SinkBuffer& buffer, std::span<s16> samples) override {
        // An audio out stream (a movie's sound, say) and the renderer's primary
        // stream are queued as on a device, and played by LibretroSink::MixOut
        // at the frontend's pace. Playing a buffer releases it, which is what
        // paces the game: the audio out service releases its buffers by the
        // samples played, and the renderer only runs a few buffers ahead of what
        // has been played. The renderer's other streams carry the same mix and
        // are dropped.
        if (type == StreamType::Out || is_primary) {
            SinkStream::AppendBuffer(buffer, samples);
        }
    }

    std::vector<s16> ReleaseBuffer(u64) override {
        return {};
    }

    StreamType Type() const {
        return type;
    }

    bool IsPrimary() const {
        return is_primary;
    }

    /// Stops the renderer waiting for this stream to be played, for good.
    void Detach() {
        detached = true;
        SignalPause();
    }

private:
    bool is_primary{false};
    std::atomic<bool> detached{false};
};

/**
 * Audio sink that hands the emulated console's mixed output to the libretro
 * frontend instead of to a host audio device.
 */
class LibretroSink final : public Sink {
public:
    explicit LibretroSink(std::string_view) {}
    ~LibretroSink() override {
        std::scoped_lock lock{InstanceMutex()};
        if (Instance() == this) {
            Instance() = nullptr;
        }
    }

    SinkStream* AcquireSinkStream(Core::System& system, u32 stream_channels, const std::string&,
                                  StreamType type) override {
        std::scoped_lock lock{InstanceMutex()};
        const bool primary = (type == StreamType::Render) && !has_primary;
        if (primary) {
            has_primary = true;
        }
        // The audio core makes an output and an input sink from the same
        // setting; MixOut plays the streams of the one that has them. Taking
        // whichever was made last pointed it at the input sink, so a game's
        // audio out was never played or released and a movie stopped after its
        // first frames.
        if (type == StreamType::Out || primary) {
            Instance() = this;
        }
        auto stream =
            std::make_unique<LibretroSinkStreamImpl>(system, type, primary, stream_channels);
        auto* raw = stream.get();
        streams.push_back(std::move(stream));
        return raw;
    }

    void CloseStream(SinkStream* stream) override {
        std::scoped_lock lock{InstanceMutex()};
        std::erase_if(streams, [stream](const SinkStreamPtr& s) { return s.get() == stream; });
        if (streams.empty()) {
            has_primary = false;
        }
    }

    void CloseStreams() override {
        std::scoped_lock lock{InstanceMutex()};
        streams.clear();
        has_primary = false;
    }

    /// From the frontend's thread: @p frames stereo frames of the renderer's
    /// output and every audio out stream played and added into @p out, as a
    /// host device's callback would. False when there is none.
    static bool MixOut(std::span<s16> out, std::size_t frames) {
        std::scoped_lock lock{InstanceMutex()};
        LibretroSink* sink = Instance();
        if (!sink) {
            return false;
        }
        bool any = false;
        std::vector<s16> played(frames * 2);
        for (const auto& stream : sink->streams) {
            const auto* libretro_stream = static_cast<const LibretroSinkStreamImpl*>(stream.get());
            if (libretro_stream->Type() != StreamType::Out && !libretro_stream->IsPrimary()) {
                continue;
            }
            stream->ProcessAudioOutAndRender(played, frames);
            for (std::size_t i = 0; i < frames * 2; i++) {
                out[i] = static_cast<s16>(std::clamp<s32>(s32(out[i]) + played[i], -32768, 32767));
            }
            any = true;
        }
        return any;
    }

    /// For when MixOut won't be called again, so the emulated system can still
    /// shut down: the renderer would otherwise wait for its buffers forever.
    static void Detach() {
        std::scoped_lock lock{InstanceMutex()};
        if (LibretroSink* sink = Instance()) {
            for (const auto& stream : sink->streams) {
                static_cast<LibretroSinkStreamImpl*>(stream.get())->Detach();
            }
        }
    }

    f32 GetDeviceVolume() const override {
        return volume;
    }
    void SetDeviceVolume(f32 volume_) override {
        volume = volume_;
    }
    void SetSystemVolume(f32 volume_) override {
        system_volume = volume_;
    }

private:
    /// Defined in libretro_sink.cpp: a header-inline static would give the
    /// audio core and the frontend separate copies.
    static LibretroSink*& Instance();
    static std::mutex& InstanceMutex();

    std::vector<SinkStreamPtr> streams;
    bool has_primary{false};
    f32 volume{1.0f};
    f32 system_volume{1.0f};
};

} // namespace AudioCore::Sink
