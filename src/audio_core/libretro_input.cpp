// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include "audio_core/libretro_input.h"
#include "citra_libretro/environment.h"
#include "common/logging/log.h"
#include "common/ring_buffer.h"
#include "libretro.h"

namespace AudioCore {

namespace {
// Global instance pointer for access from retro_run
LibRetroInput* g_libretro_input = nullptr;

using Clock = std::chrono::steady_clock;

// read_mic blocks until the requested count is queued, and RetroArch's CoreAudio FIFO
// (64 ms by default, ~3072 samples at 48 kHz) has no overflow guard. Reads therefore track
// wall-clock production while leaving this many samples queued: enough to cover one hardware
// callback (~1024 frames on iOS) so reads never wait, and far enough below the FIFO size that
// a slow frame does not overflow it.
constexpr s64 kFrontendBacklog = 1536;
// Discarded on (re)start so stale or overflowed FIFO contents never reach the game.
constexpr std::size_t kResyncDrain = 1536;
constexpr std::size_t kMaxReadPerPoll = 4096;
// A longer gap between polls (pause, state load) means the FIFO may have overflowed.
constexpr auto kResyncGap = std::chrono::milliseconds(100);
// A read_mic call slower than this had to wait for the hardware.
constexpr auto kBlockedRead = std::chrono::milliseconds(2);

// The MIC service calls Read() once per 15 samples of emulated time (GetBufferUpdatePeriod).
constexpr std::size_t kSamplesPerUpdate = 15;
} // namespace

struct LibRetroInput::Impl {
    std::optional<retro_microphone_interface> mic_interface;
    retro_microphone_t* mic_handle = nullptr;
    bool is_sampling = false;
    u8 sample_size_in_bytes = 2;

    // The rate at which the frontend actually provides samples (may differ from
    // what the 3DS mic service requested). We open the mic at this rate to avoid
    // RetroArch's internal resampler path, which has a convergence bug when
    // downsampling (ratio < 1). We resample ourselves in Read() instead.
    u32 native_sample_rate = 0;

    // Raw device-rate samples handed from PollMicrophone() to Read().
    Common::RingBuffer<s16, 8192> sample_buffer;

    // Temporary buffer for reading from frontend
    std::vector<s16> read_buffer;

    // Wall-clock pacing of frontend reads (see kFrontendBacklog).
    bool needs_resync = true;
    Clock::time_point read_anchor{};
    Clock::time_point last_poll{};
    s64 total_read = 0;

    // Streaming linear resampler state, carried across Read() calls so chunk boundaries
    // stay continuous and no fractional samples are lost.
    std::array<s16, 8192> raw_scratch{};
    s16 resample_last = 0;
    double resample_pos = 0.0;

    // Game-rate samples waiting to be handed to the MIC service.
    std::vector<s16> pending;
    std::size_t pending_offset = 0;

    void ResetStream() {
        needs_resync = true;
        total_read = 0;
        resample_last = 0;
        resample_pos = 0.0;
        pending.clear();
        pending.reserve(8192);
        pending_offset = 0;
        while (sample_buffer.Pop(raw_scratch.data(), raw_scratch.size()) > 0) {
        }
    }

    Impl() {
        // Try to get the microphone interface from the frontend
        retro_microphone_interface interface{};
        interface.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;

        if (LibRetro::GetMicrophoneInterface(&interface)) {
            if (interface.interface_version == RETRO_MICROPHONE_INTERFACE_VERSION) {
                mic_interface = interface;
                LOG_INFO(Audio, "LibRetro microphone interface available (version {})",
                         interface.interface_version);
            } else {
                LOG_WARNING(Audio,
                            "LibRetro microphone interface version mismatch: expected {}, got {}",
                            RETRO_MICROPHONE_INTERFACE_VERSION, interface.interface_version);
            }
        } else {
            LOG_WARNING(Audio, "LibRetro microphone interface not available");
        }

        read_buffer.resize(std::max(kMaxReadPerPoll, kResyncDrain));
    }

    ~Impl() {
        CloseMicrophone();
    }

    bool EnsureMicrophoneOpen() {
        if (mic_handle) {
            return true;
        }

        if (!mic_interface) {
            return false;
        }

        // Always open at 48000 Hz regardless of what the game requests.
        // RetroArch's microphone_driver_read has a resampler whose while-loop
        // deadlocks when the ratio is < 1 (core rate < device rate). The
        // libretro get_params API only returns the effective (requested) rate,
        // not the device's native rate, so we can't detect the mismatch.
        // Opening at 48000 Hz (the most common hardware rate) keeps the
        // frontend's internal resampling ratio at or near 1.0, avoiding the
        // bug. We resample to the game's requested rate ourselves in Read().
        static constexpr u32 kMicOpenRate = 48000;
        native_sample_rate = kMicOpenRate;

        retro_microphone_params_t params{};
        params.rate = kMicOpenRate;

        mic_handle = mic_interface->open_mic(&params);
        if (!mic_handle) {
            LOG_ERROR(Audio, "Failed to open LibRetro microphone");
            return false;
        }

        // The frontend may start recording immediately in open_mic (e.g.
        // CoreAudio calls AudioOutputUnitStart). Pause it right away so the
        // mic is available but idle until StartSampling enables it.
        mic_interface->set_mic_state(mic_handle, false);

        LOG_INFO(Audio, "LibRetro microphone opened at {} Hz (idle)", native_sample_rate);
        return true;
    }

    void CloseMicrophone() {
        if (mic_interface && mic_handle) {
            mic_interface->close_mic(mic_handle);
            mic_handle = nullptr;
        }
    }

    bool SetMicrophoneActive(bool active) {
        if (!mic_interface || !mic_handle) {
            return false;
        }
        return mic_interface->set_mic_state(mic_handle, active);
    }

    bool IsMicrophoneActive() const {
        if (!mic_interface || !mic_handle) {
            return false;
        }
        return mic_interface->get_mic_state(mic_handle);
    }
};

LibRetroInput::LibRetroInput() : impl(std::make_unique<Impl>()) {
    g_libretro_input = this;
}

LibRetroInput::~LibRetroInput() {
    StopSampling();
    if (g_libretro_input == this) {
        g_libretro_input = nullptr;
    }
}

void LibRetroInput::StartSampling(const InputParameters& params) {
    if (IsSampling()) {
        return;
    }

    // LibRetro only provides signed 16-bit PCM samples
    // We'll convert to the requested format in Read()
    if (params.sign == Signedness::Unsigned) {
        LOG_DEBUG(Audio, "Application requested unsigned PCM format; will convert from signed.");
    }

    parameters = params;
    impl->sample_size_in_bytes = params.sample_size / 8;

    if (!impl->EnsureMicrophoneOpen()) {
        LOG_WARNING(Audio, "Cannot start sampling: microphone not available");
        return;
    }

    // Enable the microphone (transitions from idle to recording)
    if (!impl->SetMicrophoneActive(true)) {
        LOG_ERROR(Audio, "Failed to activate microphone");
        return;
    }

    impl->ResetStream();
    impl->is_sampling = true;
    LOG_INFO(Audio, "LibRetro microphone sampling started at {} Hz, {} bit", params.sample_rate,
             params.sample_size);
}

void LibRetroInput::StopSampling() {
    if (!impl->is_sampling) {
        return;
    }

    impl->SetMicrophoneActive(false);
    impl->is_sampling = false;

    LOG_INFO(Audio, "LibRetro microphone sampling stopped (mic remains idle)");
}

bool LibRetroInput::IsSampling() {
    return impl->is_sampling;
}

void LibRetroInput::AdjustSampleRate(u32 sample_rate) {
    if (!IsSampling()) {
        return;
    }

    // Restart with new sample rate
    auto new_parameters = parameters;
    new_parameters.sample_rate = sample_rate;
    StopSampling();
    StartSampling(new_parameters);
}

void LibRetroInput::PollMicrophone() {
    // This is called from the main thread (retro_run)
    // Read samples from the frontend and push to the ring buffer

    if (!impl->is_sampling || !impl->mic_interface || !impl->mic_handle) {
        return;
    }

    // Issue a memory fence before reading. RetroArch's CoreAudio mic driver
    // fills its FIFO from a callback thread without memory barriers. On ARM64
    // (weak memory model), the main thread may not see the callback's writes
    // without an explicit barrier.
    std::atomic_thread_fence(std::memory_order_acquire);

    const auto now = Clock::now();
    if (impl->needs_resync || now - impl->last_poll > kResyncGap) {
        // One-time wait of at most ~32 ms when sampling starts or resumes.
        impl->mic_interface->read_mic(impl->mic_handle, impl->read_buffer.data(), kResyncDrain);
        impl->read_anchor = Clock::now();
        impl->last_poll = impl->read_anchor;
        // Leave the next kFrontendBacklog samples queued as the safety margin.
        impl->total_read = kFrontendBacklog;
        impl->needs_resync = false;
        return;
    }
    impl->last_poll = now;

    // Read exactly what the device has produced since the anchor, minus the backlog.
    const double elapsed = std::chrono::duration<double>(now - impl->read_anchor).count();
    const s64 produced = static_cast<s64>(elapsed * impl->native_sample_rate);
    const s64 wanted = produced - impl->total_read;
    if (wanted <= 0) {
        return;
    }
    const auto to_read = static_cast<std::size_t>(
        std::min<s64>(wanted, static_cast<s64>(kMaxReadPerPoll)));

    const auto read_start = Clock::now();
    const int samples_read =
        impl->mic_interface->read_mic(impl->mic_handle, impl->read_buffer.data(), to_read);
    const auto read_time = Clock::now() - read_start;

    if (samples_read > 0) {
        impl->total_read += samples_read;
        impl->sample_buffer.Push(
            std::span<const s16>(impl->read_buffer.data(), static_cast<size_t>(samples_read)));
    }
    if (read_time > kBlockedRead) {
        // The hardware delivered later than the wall clock predicted; shift the anchor so the
        // following reads keep a full backlog instead of waiting again.
        impl->read_anchor += read_time;
    }
}

Samples LibRetroInput::Read() {
    // This is called from the CoreTiming scheduler thread
    // Pop samples from the ring buffer (thread-safe)

    if (!impl->is_sampling) {
        return {};
    }

    auto& pending = impl->pending;

    // Resample newly polled device-rate samples to the game rate.
    const std::size_t raw_count =
        impl->sample_buffer.Pop(impl->raw_scratch.data(), impl->raw_scratch.size());
    if (raw_count > 0) {
        const s16* raw = impl->raw_scratch.data();
        if (impl->native_sample_rate == 0 || impl->native_sample_rate == parameters.sample_rate) {
            pending.insert(pending.end(), raw, raw + raw_count);
        } else {
            // Position is in input samples; index 0 is the last sample of the previous chunk.
            const double step =
                static_cast<double>(impl->native_sample_rate) / parameters.sample_rate;
            double pos = impl->resample_pos;
            while (pos < static_cast<double>(raw_count)) {
                const auto idx = static_cast<std::size_t>(pos);
                const double frac = pos - static_cast<double>(idx);
                const s16 s0 = idx == 0 ? impl->resample_last : raw[idx - 1];
                const s16 s1 = raw[idx];
                pending.push_back(static_cast<s16>(s0 + (s1 - s0) * frac));
                pos += step;
            }
            impl->resample_pos = pos - static_cast<double>(raw_count);
            impl->resample_last = raw[raw_count - 1];
        }
    }

    // Hand samples over at the rate the real MIC hardware fills its buffer. Catch up if the
    // queue grows past ~2 frames (e.g. emulation below full speed) instead of adding latency.
    const std::size_t available = pending.size() - impl->pending_offset;
    if (available == 0) {
        return {};
    }
    const std::size_t high_water = parameters.sample_rate / 30;
    std::size_t count = std::min(available, kSamplesPerUpdate);
    if (available > high_water) {
        count = available - high_water + kSamplesPerUpdate;
    }

    const std::span<const s16> raw_samples(pending.data() + impl->pending_offset, count);

    // Convert sample format if needed
    constexpr auto convert_s16_to_u16 = [](s16 sample) -> u16 {
        return static_cast<u16>(sample) ^ 0x8000;
    };

    constexpr auto convert_s16_to_s8 = [](s16 sample) -> s8 {
        return static_cast<s8>(sample >> 8);
    };

    constexpr auto convert_s16_to_u8 = [](s16 sample) -> u8 {
        return static_cast<u8>((static_cast<u16>(sample) ^ 0x8000) >> 8);
    };

    Samples output;
    output.reserve(raw_samples.size() * impl->sample_size_in_bytes);

    if (impl->sample_size_in_bytes == 1) {
        // 8-bit output
        if (parameters.sign == Signedness::Unsigned) {
            for (s16 sample : raw_samples) {
                output.push_back(convert_s16_to_u8(sample));
            }
        } else {
            for (s16 sample : raw_samples) {
                output.push_back(static_cast<u8>(convert_s16_to_s8(sample)));
            }
        }
    } else {
        // 16-bit output
        if (parameters.sign == Signedness::Unsigned) {
            for (s16 sample : raw_samples) {
                u16 converted = convert_s16_to_u16(sample);
                output.push_back(static_cast<u8>(converted & 0xFF));
                output.push_back(static_cast<u8>(converted >> 8));
            }
        } else {
            // Signed 16-bit - just copy the raw bytes
            const u8* data = reinterpret_cast<const u8*>(raw_samples.data());
            output.insert(output.end(), data, data + raw_samples.size() * 2);
        }
    }

    // raw_samples points into pending, so only advance the queue after conversion.
    impl->pending_offset += count;
    if (impl->pending_offset == pending.size()) {
        pending.clear();
        impl->pending_offset = 0;
    } else if (impl->pending_offset > 4096) {
        pending.erase(pending.begin(),
                      pending.begin() + static_cast<std::ptrdiff_t>(impl->pending_offset));
        impl->pending_offset = 0;
    }

    return output;
}

LibRetroInput* GetLibRetroInput() {
    return g_libretro_input;
}

} // namespace AudioCore
