// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_libretro/audio/libretro_input.h"
#include "citra_libretro/environment.h"
#include "common/logging/log.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace LibRetro::Audio {

// Global microphone interface
static retro_microphone_interface g_microphone_interface = {0};
static bool g_microphone_initialized = false;

bool InitializeMicrophoneInterface() {
    if (g_microphone_initialized) {
        return true;
    }

    std::memset(&g_microphone_interface, 0, sizeof(g_microphone_interface));
    g_microphone_interface.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;

    if (!LibRetro::GetMicrophoneInterface(&g_microphone_interface)) {
        LOG_WARNING(Service_MIC, "LibRetro: Frontend does not support microphone interface");
        return false;
    }

    g_microphone_initialized = true;
    LOG_INFO(Service_MIC, "LibRetro: Microphone interface initialized successfully (version {})", 
             g_microphone_interface.interface_version);
    return true;
}

void ShutdownMicrophoneInterface() {
    g_microphone_initialized = false;
    std::memset(&g_microphone_interface, 0, sizeof(g_microphone_interface));
}

LibRetroInput::LibRetroInput() {
    LOG_INFO(Service_MIC, "LibRetro: Microphone Creating LibRetroInput");
    
    if (!g_microphone_initialized) {
        InitializeMicrophoneInterface();
    }
    
    if (g_microphone_initialized) {
        std::memcpy(&microphone_interface, &g_microphone_interface, sizeof(microphone_interface));
    }
}

LibRetroInput::~LibRetroInput() {
    StopSampling();
}

void LibRetroInput::StartSampling(const AudioCore::InputParameters& params) {
    if (!g_microphone_initialized || is_sampling) {
        return;
    }
    LOG_INFO(Service_MIC, "LibRetro: Microphone Starting sampling - sample_rate: {}, buffer_size: {} bytes ({} samples), sample_size: {} bits", 
             params.sample_rate, params.buffer_size, params.buffer_size / 2, params.sample_size);
    
    // Sanity check: if buffer_size is unreasonably large, cap it
    AudioCore::InputParameters adjusted_params = params;
    if (adjusted_params.buffer_size > 16384) {  // 16KB max (8192 samples at 16-bit)
        LOG_WARNING(Service_MIC, "LibRetro: Buffer size {} is too large, capping to 16384", 
                    adjusted_params.buffer_size);
        adjusted_params.buffer_size = 16384;
    }
    
    parameters = adjusted_params;

    // Open microphone if not already open
    if (microphone_handle == nullptr && microphone_interface.open_mic) {
        retro_microphone_params_t mic_params = {0};
        mic_params.rate = params.sample_rate;
        
        microphone_handle = microphone_interface.open_mic(&mic_params);
        
        if (microphone_handle) {
            LOG_INFO(Service_MIC, "LibRetro: Microphone opened successfully (rate: {}Hz)", 
                     params.sample_rate);
            
            // Activate microphone
            if (microphone_interface.set_mic_state) {
                microphone_interface.set_mic_state(microphone_handle, true);
                is_sampling = true;
                LOG_INFO(Service_MIC, "LibRetro: Microphone sampling started");
            }
        } else {
            LOG_ERROR(Service_MIC, "LibRetro: Microphone Failed to open microphone");
        }
    }
}

void LibRetroInput::StopSampling() {
    if (!is_sampling) {
        return;
    }
    LOG_INFO(Service_MIC, "LibRetro: Microphone Stopping sampling");
    is_sampling = false;

    // Deactivate and close microphone
    if (microphone_handle) {
        if (microphone_interface.set_mic_state) {
            microphone_interface.set_mic_state(microphone_handle, false);
        }
        
        if (microphone_interface.close_mic) {
            microphone_interface.close_mic(microphone_handle);
        }
        
        microphone_handle = nullptr;
        LOG_INFO(Service_MIC, "LibRetro: Microphone sampling stopped");
    }
}

bool LibRetroInput::IsSampling() {
    return is_sampling;
}

void LibRetroInput::AdjustSampleRate(u32 sample_rate) {
    parameters.sample_rate = sample_rate;
    
    // Need to reopen microphone with new sample rate
    if (is_sampling) {
        StopSampling();
        StartSampling(parameters);
    }
}

AudioCore::Samples LibRetroInput::Read() {
    if (!is_sampling || !microphone_handle || !microphone_interface.read_mic) {
        return {};
    }

    // LibRetro microphone interface always returns 16-bit signed PCM samples
    // Read a reasonable chunk size per call
    // For 32768 Hz at ~60 fps: 32768/60 = ~546 samples per frame
    // Use a moderate buffer size that balances latency and CPU usage
    // 256 samples = ~7.8ms at 32768 Hz, good balance for real-time audio
    constexpr size_t max_samples = 32;
    std::vector<int16_t> mic_samples(max_samples);
    
    int samples_read = microphone_interface.read_mic(microphone_handle, 
                                                      mic_samples.data(), 
                                                      max_samples);
    
    if (samples_read <= 0) {
        return {};
    }

    // Calculate the number of bytes needed
    u8 sample_size_in_bytes = parameters.sample_size / 8;
    AudioCore::Samples samples(samples_read * sample_size_in_bytes);
    
    if (parameters.sample_size == 8) {
        // 8-bit output
        if (parameters.sign == AudioCore::Signedness::Unsigned) {
            // OpenAL format: AL_FORMAT_MONO8 (unsigned 8-bit)
            // Convert 16-bit signed to 8-bit unsigned
            for (int i = 0; i < samples_read; i++) {
                // Scale from [-32768, 32767] to [0, 255]
                int32_t temp = (int32_t)mic_samples[i] + 32768;
                samples[i] = static_cast<u8>(temp >> 8);
            }
        } else {
            // Signed 8-bit (not commonly used by OpenAL, but supported by 3DS)
            for (int i = 0; i < samples_read; i++) {
                samples[i] = static_cast<u8>(mic_samples[i] >> 8);
            }
        }
    } else {
        // 16-bit output
        if (parameters.sign == AudioCore::Signedness::Signed) {
            // OpenAL format: AL_FORMAT_MONO16 (signed 16-bit)
            // Direct byte copy - libretro mic returns signed 16-bit PCM
            std::memcpy(samples.data(), mic_samples.data(), samples_read * 2);
        } else {
            // Unsigned 16-bit (not commonly used by OpenAL, but supported by 3DS)
            for (int i = 0; i < samples_read; i++) {
                int16_t sample = mic_samples[i];
                // Convert signed to unsigned
                uint16_t unsigned_sample = static_cast<uint16_t>((int32_t)sample + 32768);
                // Store as little-endian bytes
                samples[i * 2] = static_cast<u8>(unsigned_sample & 0xFF);
                samples[i * 2 + 1] = static_cast<u8>(unsigned_sample >> 8);
            }
        }
    }

    return samples;
}

} // namespace LibRetro::Audio
