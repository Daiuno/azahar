// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include "audio_core/input.h"
#include "libretro.h"

namespace LibRetro::Audio {

/**
 * A microphone input implementation that receives audio from the libretro frontend's microphone driver.
 */
class LibRetroInput final : public AudioCore::Input {
public:
    LibRetroInput();
    ~LibRetroInput() override;

    void StartSampling(const AudioCore::InputParameters& params) override;
    void StopSampling() override;
    bool IsSampling() override;
    void AdjustSampleRate(u32 sample_rate) override;
    AudioCore::Samples Read() override;

private:
    bool is_sampling = false;
    retro_microphone_t* microphone_handle = nullptr;
    retro_microphone_interface microphone_interface = {};
};

/// Initialize the libretro microphone interface
bool InitializeMicrophoneInterface();

/// Shutdown the libretro microphone interface
void ShutdownMicrophoneInterface();

} // namespace LibRetro::Audio
