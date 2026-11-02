// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Core {

// Owned by one ArmRecomp::Impl. Sample only at execution boundaries on that
// core, never from a host polling thread. No process, thread, memory pointer,
// or callback survives Sample(). Destruction therefore cannot race a poll.
// These are last-observed samples, NOT a live enumeration of guest threads.
class RecompDiagnosticSampler {
public:
    using Clock = std::chrono::steady_clock;
    struct Config {
        bool snapshots = false;
        bool watch = false;
        std::uint64_t lr_offset = 0;
        std::uint64_t fixed_address = 0;
        std::string watch_path;
    };

    void Configure(std::uint64_t process_id, std::size_t core, Config config) {
        config_ = std::move(config);
        process_id_ = process_id;
        core_ = core;
        // Separate streams for every process/core; do not concurrently truncate
        // the same user-specified file from multiple CPU interfaces.
        if (!config_.watch_path.empty()) {
            config_.watch_path += ".p" + std::to_string(process_id_) + ".core" +
                                  std::to_string(core_);
        }
    }

    void TrackSlot(std::uint64_t address, std::uint64_t stub, std::string name) {
        for (const auto& slot : slots_) {
            if (slot.address == address) return;
        }
        slots_.push_back({address, stub, std::move(name)});
    }

    bool Enabled() const {
        return config_.snapshots || config_.watch || !slots_.empty();
    }

    // now is injectable for deterministic tests. Readers and publishers must
    // execute synchronously; the caller supplies its currently running context.
    template <typename Modules, typename Read32, typename Read64, typename IsMapped,
              typename Publish, typename Emit>
    void Sample(Clock::time_point now, std::uint64_t thread_id, std::uint64_t pc,
                std::uint64_t lr, std::uint64_t x19, const Modules& modules,
                Read32&& read32, Read64&& read64, IsMapped&& mapped,
                Publish&& publish, Emit&& emit) {
        if (!Enabled() || (started_ && now < next_sample_)) return;
        if (!started_) {
            started_ = true;
            start_ = now;
            next_slots_ = now;
            next_heartbeat_ = now;
        }
        next_sample_ = now + std::chrono::milliseconds(100);
        ++samples_;
        const auto valid_range = [&](std::uint64_t address, std::uint64_t size) {
            return size != 0 && address <= std::numeric_limits<std::uint64_t>::max() - (size - 1) &&
                   mapped(address) && mapped(address + size - 1);
        };
        const auto prefix = [&] {
            std::ostringstream s;
            s << "recomp sample process=" << process_id_ << " core=" << core_
              << " thread=" << thread_id << " t="
              << std::chrono::duration<double>(now - start_).count()
              << " pc=0x" << std::hex << pc << " lr=0x" << lr;
            return s.str();
        };
        const auto write = [&](const std::string& line) {
            emit(line);
            if (config_.watch && !config_.watch_path.empty()) {
                if (!file_attempted_) {
                    file_attempted_ = true;
                    output_.open(config_.watch_path, std::ios::trunc);
                    if (!output_) emit("recomp: cannot open diagnostic file " + config_.watch_path);
                }
                if (output_) {
                    output_ << line << '\n';
                    output_.flush();
                }
            }
        };
        if (config_.snapshots || config_.watch) {
            publish(prefix()); // copied text only; no borrowed kernel objects
        }
        if (now >= next_slots_) {
            next_slots_ = now + std::chrono::seconds(1);
            std::size_t bound = 0, readable = 0;
            for (const auto& slot : slots_) {
                if (!valid_range(slot.address, 8)) continue;
                ++readable;
                if (read64(slot.address) != slot.stub) ++bound;
            }
            if (!slots_.empty() && (bound != last_bound_ || readable != last_readable_)) {
                write(prefix() + " slots_bound=" + std::to_string(bound) +
                      " readable=" + std::to_string(readable) +
                      " tracked=" + std::to_string(slots_.size()));
                last_bound_ = bound;
                last_readable_ = readable;
            }
        }
        if (!config_.watch) return;
        // Re-evaluate the object for this executing thread. Do not retain x19
        // from an unrelated thread or assume a previously mapped object lives.
        std::uint64_t base = config_.fixed_address;
        if (!base) {
            for (const auto& [module_base, name] : modules) {
                (void)name;
                if (lr >= module_base && lr - module_base == config_.lr_offset) {
                    base = x19;
                    break;
                }
            }
        }
        if (!base || base > std::numeric_limits<std::uint64_t>::max() - 0x10f ||
            !valid_range(base + 0xd0, 0x40)) {
            have_window_ = false;
            return;
        }
        std::array<std::uint32_t, 16> window{};
        for (std::size_t i = 0; i < window.size(); ++i) {
            window[i] = read32(base + 0xd0 + 4 * i);
        }
        if (!have_window_ || base != last_base_ || window != previous_ || now >= next_heartbeat_) {
            std::ostringstream s;
            s << prefix() << " object=0x" << std::hex << base << " window=";
            for (const auto word : window) s << ' ' << std::hex << word;
            s << " samples=" << std::dec << samples_;
            write(s.str());
            previous_ = window;
            last_base_ = base;
            have_window_ = true;
            next_heartbeat_ = now + std::chrono::seconds(5);
        }
    }

private:
    struct Slot { std::uint64_t address, stub; std::string name; };
    Config config_;
    std::uint64_t process_id_ = 0, last_base_ = 0, samples_ = 0;
    std::size_t core_ = 0;
    std::vector<Slot> slots_;
    std::array<std::uint32_t, 16> previous_{};
    std::size_t last_bound_ = std::numeric_limits<std::size_t>::max();
    std::size_t last_readable_ = std::numeric_limits<std::size_t>::max();
    bool started_ = false, have_window_ = false, file_attempted_ = false;
    Clock::time_point start_{}, next_sample_{}, next_slots_{}, next_heartbeat_{};
    std::ofstream output_;
};

} // namespace Core
