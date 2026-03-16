// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <utility>
#include <vector>
#include <AL/al.h>
#include <AL/alc.h>
#include "audio_core/input.h"
#include "audio_core/openal_input.h"
#include "audio_core/sink.h"
#include "common/logging/log.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_TV
#import <AVFoundation/AVFoundation.h>

// iOS 特定的音频会话配置
static bool ConfigureIOSAudioSessionForCapture() {
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;
        
        // 设置音频会话类别为 PlayAndRecord,这是捕获音频所必需的
        BOOL success = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                        mode:AVAudioSessionModeDefault
                                     options:AVAudioSessionCategoryOptionDefaultToSpeaker |
                                             AVAudioSessionCategoryOptionAllowBluetooth
                                       error:&error];
        if (!success || error) {
            NSLog(@"Failed to set audio session category: %@", error);
            return false;
        }
        
        // 激活音频会话
        success = [session setActive:YES error:&error];
        if (!success || error) {
            NSLog(@"Failed to activate audio session: %@", error);
            return false;
        }
        
        NSLog(@"Successfully configured iOS audio session for capture");
        return true;
    }
}
#endif
#endif

namespace AudioCore {

struct OpenALInput::Impl {
    ALCdevice* device = nullptr;
    u8 sample_size_in_bytes = 0;
};

OpenALInput::OpenALInput(std::string device_id)
    : impl(std::make_unique<Impl>()), device_id(std::move(device_id)) {}

OpenALInput::~OpenALInput() {
    StopSampling();
}

void OpenALInput::StartSampling(const InputParameters& params) {
    if (IsSampling()) {
        return;
    }

#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV
    // iOS 必须在打开捕获设备之前配置音频会话
    if (!ConfigureIOSAudioSessionForCapture()) {
        LOG_CRITICAL(Audio, "Failed to configure iOS audio session for capture");
        return;
    }
#endif
#endif

    // OpenAL supports unsigned 8-bit and signed 16-bit PCM.
    // TODO: Re-sample the stream.
    if ((params.sample_size == 8 && params.sign == Signedness::Signed) ||
        (params.sample_size == 16 && params.sign == Signedness::Unsigned)) {
        LOG_WARNING(Audio, "Application requested unsupported unsigned PCM format. Falling back to "
                           "supported format.");
    }

    parameters = params;
    impl->sample_size_in_bytes = params.sample_size / 8;

    auto format = params.sample_size == 16 ? AL_FORMAT_MONO16 : AL_FORMAT_MONO8;
    
    // iOS 特殊处理:列出可用的捕获设备并记录调试信息
    const char* default_device = nullptr;
    if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT") != AL_FALSE) {
        default_device = alcGetString(nullptr, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
        LOG_INFO(Audio, "Default capture device: {}", default_device ? default_device : "null");
        
        const char* devices_str = alcGetString(nullptr, ALC_CAPTURE_DEVICE_SPECIFIER);
        if (devices_str && *devices_str != '\0') {
            LOG_INFO(Audio, "Available capture devices:");
            while (*devices_str != '\0') {
                LOG_INFO(Audio, "  - {}", devices_str);
                devices_str += strlen(devices_str) + 1;
            }
        } else {
            LOG_WARNING(Audio, "No capture devices enumerated");
        }
    } else {
        LOG_WARNING(Audio, "ALC_ENUMERATION_EXT not present");
    }
    
    // 对于 iOS,强制使用默认设备(nullptr)而不是 "auto" 字符串
    // iOS 上的 OpenAL Soft 不接受任何设备名称字符串,必须使用 nullptr
    const char* device_name = nullptr;
    #if !defined(__APPLE__) || defined(__MACOSX)
    // 仅在非 iOS 平台上使用指定的设备名称
    if (device_id != auto_device_name && !device_id.empty()) {
        device_name = device_id.c_str();
    }
    #endif
    
    LOG_INFO(Audio, "Attempting to open capture device with name: {}, sample_rate: {}, format: {}, buffer_size: {}", 
             device_name ? device_name : "nullptr (default)", 
             params.sample_rate, 
             format == AL_FORMAT_MONO16 ? "MONO16" : "MONO8",
             params.buffer_size);
    
    impl->device = alcCaptureOpenDevice(
        device_name,
        params.sample_rate, format, static_cast<ALsizei>(params.buffer_size));
    auto open_error = alcGetError(impl->device);
    if (impl->device == nullptr || open_error != ALC_NO_ERROR) {
        LOG_CRITICAL(Audio, "alcCaptureOpenDevice failed: error code {:#x}", open_error);
        StopSampling();
        return;
    }

    alcCaptureStart(impl->device);
    auto capture_error = alcGetError(impl->device);
    if (capture_error != ALC_NO_ERROR) {
        LOG_CRITICAL(Audio, "alcCaptureStart failed: error code {:#x}", capture_error);
        StopSampling();
        return;
    }
    
    LOG_INFO(Audio, "Successfully started audio capture");
}

void OpenALInput::StopSampling() {
    if (impl->device) {
        alcCaptureStop(impl->device);
        alcCaptureCloseDevice(impl->device);
        impl->device = nullptr;
    }
}

bool OpenALInput::IsSampling() {
    return impl->device != nullptr;
}

void OpenALInput::AdjustSampleRate(u32 sample_rate) {
    if (!IsSampling()) {
        return;
    }

    auto new_params = parameters;
    new_params.sample_rate = sample_rate;
    StopSampling();
    StartSampling(new_params);
}

Samples OpenALInput::Read() {
    if (!IsSampling()) {
        return {};
    }

    ALCint samples_captured = 0;
    alcGetIntegerv(impl->device, ALC_CAPTURE_SAMPLES, 1, &samples_captured);
    auto error = alcGetError(impl->device);
    if (error != ALC_NO_ERROR) {
        LOG_WARNING(Audio, "alcGetIntegerv(ALC_CAPTURE_SAMPLES) failed: {}", error);
        return {};
    }

    auto num_samples = std::min(samples_captured, static_cast<ALsizei>(parameters.buffer_size /
                                                                       impl->sample_size_in_bytes));
    Samples samples(num_samples * impl->sample_size_in_bytes);

    alcCaptureSamples(impl->device, samples.data(), num_samples);
    error = alcGetError(impl->device);
    if (error != ALC_NO_ERROR) {
        LOG_WARNING(Audio, "alcCaptureSamples failed: {}", error);
        return {};
    }

    return samples;
}

std::vector<std::string> ListOpenALInputDevices() {
    const char* devices_str;
    if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT") != AL_FALSE) {
        devices_str = alcGetString(nullptr, ALC_CAPTURE_DEVICE_SPECIFIER);
    } else {
        LOG_WARNING(
            Audio,
            "Missing OpenAL device enumeration extensions, cannot list audio capture devices.");
        return {};
    }

    if (!devices_str || *devices_str == '\0') {
        return {};
    }

    std::vector<std::string> device_list;
    while (*devices_str != '\0') {
        device_list.emplace_back(devices_str);
        devices_str += strlen(devices_str) + 1;
    }
    return device_list;
}

} // namespace AudioCore
