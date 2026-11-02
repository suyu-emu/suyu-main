// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
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

/**
 * Sample queue shared between the emulated audio renderer (producer, running
 * on suyu's audio thread) and the libretro frontend (consumer, draining once
 * per retro_run on RetroArch's thread).
 *
 * A plain locked deque rather than a lock-free ring: the producer appends in
 * large infrequent chunks and the consumer drains everything at 60Hz, so
 * contention is negligible and the simplicity is worth more than the
 * micro-optimisation.
 */
class LibretroSampleQueue {
public:
    /// Defined in libretro_sink.cpp, not here: a header-inline static would
    /// give the producer and consumer separate copies of the queue.
    static LibretroSampleQueue& Instance();

    void Push(std::span<const s16> samples) {
        std::scoped_lock lock{mutex};
        // Hard cap the backlog, and keep it short. The renderer hands us audio
        // in bursts rather than a steady stream (nothing paces it here the way
        // a real device would), so a large cap just turns into seconds of
        // latency delivered as one lump, which the frontend then drops. A
        // quarter second is enough to ride out the bursts while keeping audio
        // in step with video; anything older than that is stale and is dropped
        // oldest-first.
        constexpr size_t kMaxQueuedSamples = 48000 / 4 * 2; // 0.25s stereo
        if (queue.size() + samples.size() > kMaxQueuedSamples) {
            const size_t overflow = queue.size() + samples.size() - kMaxQueuedSamples;
            const size_t to_drop = std::min(overflow, queue.size());
            queue.erase(queue.begin(), queue.begin() + static_cast<ptrdiff_t>(to_drop));
        }
        queue.insert(queue.end(), samples.begin(), samples.end());
    }

    /// Moves every queued sample into @p out, leaving the queue empty.
    void Drain(std::vector<s16>& out) {
        std::scoped_lock lock{mutex};
        out.assign(queue.begin(), queue.end());
        queue.clear();
    }

    void Clear() {
        std::scoped_lock lock{mutex};
        queue.clear();
    }

private:
    std::mutex mutex;
    std::vector<s16> queue;
};

class LibretroSinkStreamImpl final : public SinkStream {
public:
    explicit LibretroSinkStreamImpl(Core::System& system_, StreamType type_, bool is_primary_,
                                    u32 system_channels_)
        : SinkStream{system_, type_}, is_primary{is_primary_} {
        system_channels = system_channels_;
        device_channels = 2;
    }
    ~LibretroSinkStreamImpl() override = default;

    void AppendBuffer(SinkBuffer& buffer, std::span<s16> samples) override {
        // An audio out stream (a movie's sound, say) is queued as on a device,
        // and played by LibretroSink::MixOut at the frontend's pace: the audio
        // out service releases a game's buffers by the samples played, and
        // dropped here they were never released, so the game waited on them
        // and its movie stopped after a few frames.
        if (type == StreamType::Out) {
            SinkStream::AppendBuffer(buffer, samples);
            return;
        }
        // Only the primary stream feeds the frontend. The renderer acquires
        // more than one output stream and they all carry the same mix, so
        // pushing every one produced audio at a multiple of real time, which
        // overran the queue and made it dump ~2s at once. Note the streams
        // arrive as StreamType::Render, not Out - filtering on Out matched
        // nothing at all, which is why the core was previously silent.
        if (is_primary && type != StreamType::In) {
            LibretroSampleQueue::Instance().Push(samples);
        }
    }

    std::vector<s16> ReleaseBuffer(u64) override {
        return {};
    }

    StreamType Type() const {
        return type;
    }

private:
    bool is_primary{false};
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
        // The audio core makes an output and an input sink from the same
        // setting; MixOut plays the audio out streams of the one that has them.
        // Taking whichever was made last pointed it at the input sink, so a
        // game's audio out was never played or released and a movie stopped
        // after its first frames.
        if (type == StreamType::Out) {
            Instance() = this;
        }
        // The renderer's first stream feeds LibretroSampleQueue; audio out
        // streams are played by MixOut
        const bool primary = (type == StreamType::Render) && !has_primary;
        if (primary) {
            has_primary = true;
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

    /// From the frontend's thread: @p frames stereo frames of every audio out
    /// stream played and added into @p out. False when there is none.
    static bool MixOut(std::span<s16> out, std::size_t frames) {
        std::scoped_lock lock{InstanceMutex()};
        LibretroSink* sink = Instance();
        if (!sink) {
            return false;
        }
        bool any = false;
        std::vector<s16> played(frames * 2);
        for (const auto& stream : sink->streams) {
            if (static_cast<const LibretroSinkStreamImpl*>(stream.get())->Type() != StreamType::Out) {
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
    /// Defined in libretro_sink.cpp, like LibretroSampleQueue::Instance
    static LibretroSink*& Instance();
    static std::mutex& InstanceMutex();

    std::vector<SinkStreamPtr> streams;
    bool has_primary{false};
    f32 volume{1.0f};
    f32 system_volume{1.0f};
};

} // namespace AudioCore::Sink
