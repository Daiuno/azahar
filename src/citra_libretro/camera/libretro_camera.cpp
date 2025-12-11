// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include "citra_libretro/camera/libretro_camera.h"
#include "citra_libretro/environment.h"
#include "common/logging/log.h"

namespace LibRetro::Camera {

// Global camera state
static CameraState g_camera_state;

CameraState& GetCameraState() {
    return g_camera_state;
}

// Callback function that receives frames from libretro frontend
static void CameraFrameCallback(const uint32_t* buffer, unsigned width, unsigned height,
                                size_t pitch) {
    // Debug: Log callback invocation (only occasionally)
    static int callback_count = 0;
    if (++callback_count % 60 == 0) {
        LOG_DEBUG(Service_CAM, "LibRetro: CameraFrameCallback {} ({}x{}, pitch={})", 
                  callback_count, width, height, pitch);
    }
    LibRetroCamera::OnCameraFrame(buffer, width, height, pitch);
}

bool InitializeCameraInterface() {
    auto& state = GetCameraState();

    if (state.camera_initialized) {
        return true;
    }

    std::memset(&state.callback, 0, sizeof(state.callback));
    state.callback.caps = 1 << RETRO_CAMERA_BUFFER_RAW_FRAMEBUFFER;
    state.callback.width = DEFAULT_CAMERA_WIDTH;
    state.callback.height = DEFAULT_CAMERA_HEIGHT;
    state.callback.frame_raw_framebuffer = CameraFrameCallback;

    if (!LibRetro::GetCameraInterface(&state.callback)) {
        LOG_WARNING(Service_CAM, "LibRetro: Frontend does not support camera interface");
        return false;
    }

    state.camera_initialized = true;
    LOG_INFO(Service_CAM, "LibRetro: Camera interface initialized successfully");
    return true;
}

void ShutdownCameraInterface() {
    auto& state = GetCameraState();

    if (state.camera_initialized && state.callback.stop) {
        state.callback.stop();
    }

    state.camera_initialized = false;
    state.frame_available = false;
    state.frame_buffer.clear();
    LOG_INFO(Service_CAM, "LibRetro: Camera interface shutdown");
}

LibRetroCamera::LibRetroCamera(const Service::CAM::Flip& flip, int cam_id)
    : flip_mode(flip), camera_id(cam_id) {
    LOG_INFO(Service_CAM, "LibRetro: Creating LibRetroCamera (camera_id: {})", camera_id);
}

LibRetroCamera::~LibRetroCamera() {
    StopCapture();
}

void LibRetroCamera::StartCapture() {
    auto& state = GetCameraState();

    if (!state.camera_initialized) {
        LOG_WARNING(Service_CAM, "LibRetro: Camera not initialized, cannot start capture");
        return;
    }

    if (capturing) {
        return;
    }

    // Switch to appropriate camera based on 3DS camera ID before starting capture
    // InnerCamera (1) = front camera, OuterRight/OuterLeft (0, 2) = back camera
    if (camera_id >= 0) {
        bool use_front = (camera_id == 1);  // InnerCamera = front
        LOG_INFO(Service_CAM, "LibRetro: Switching to {} camera for camera_id {}",
                 use_front ? "front" : "back", camera_id);
        LibRetro::SwitchCamera(use_front);
    }

    if (state.callback.start && state.callback.start()) {
        capturing = true;
        LOG_INFO(Service_CAM, "LibRetro: Camera capture started");
    } else {
        LOG_ERROR(Service_CAM, "LibRetro: Failed to start camera capture");
    }
}

void LibRetroCamera::StopCapture() {
    auto& state = GetCameraState();

    if (!capturing) {
        return;
    }

    if (state.callback.stop) {
        state.callback.stop();
    }

    capturing = false;
    LOG_INFO(Service_CAM, "LibRetro: Camera capture stopped");
}

void LibRetroCamera::SetResolution(const Service::CAM::Resolution& resolution) {
    output_width = resolution.width;
    output_height = resolution.height;
    LOG_DEBUG(Service_CAM, "LibRetro: SetResolution {}x{}", output_width, output_height);
}

void LibRetroCamera::SetFlip(Service::CAM::Flip flip) {
    flip_mode = flip;
}

void LibRetroCamera::SetEffect(Service::CAM::Effect effect) {
    // Effects not implemented
}

void LibRetroCamera::SetFormat(Service::CAM::OutputFormat format) {
    output_rgb = (format == Service::CAM::OutputFormat::RGB565);
}

void LibRetroCamera::SetFrameRate(Service::CAM::FrameRate frame_rate) {
    // Frame rate control not implemented
}

void LibRetroCamera::OnCameraFrame(const uint32_t* buffer, unsigned width, unsigned height,
                                   size_t pitch) {
    auto& state = GetCameraState();

    std::lock_guard<std::mutex> lock(state.frame_mutex);

    // pitch is in bytes, convert to pixel count
    size_t pitch_pixels = pitch / sizeof(uint32_t);

    // Resize buffer if needed
    state.frame_buffer.resize(width * height);
    state.frame_width = width;
    state.frame_height = height;

    // Copy frame data (handle pitch)
    for (unsigned y = 0; y < height; y++) {
        std::memcpy(state.frame_buffer.data() + y * width, buffer + y * pitch_pixels,
                    width * sizeof(uint32_t));
    }

    state.frame_available = true;
    
    // Debug: Log frame reception (only occasionally to avoid spam)
    static int frame_count = 0;
    if (++frame_count % 60 == 0) {
        LOG_DEBUG(Service_CAM, "LibRetro: Received frame {} ({}x{})", frame_count, width, height);
    }
}

u16 LibRetroCamera::XRGB8888ToRGB565(u32 pixel) {
    // Input format from avfoundation is BGRA
    // In memory: [B, G, R, A], as uint32_t on little-endian: 0xAARRGGBB
    // This matches the standard XRGB8888 format where X is the alpha channel
    u8 r = (pixel >> 16) & 0xFF;
    u8 g = (pixel >> 8) & 0xFF;
    u8 b = pixel & 0xFF;

    // Convert to RGB565
    u16 r5 = (r >> 3) & 0x1F;
    u16 g6 = (g >> 2) & 0x3F;
    u16 b5 = (b >> 3) & 0x1F;

    return (r5 << 11) | (g6 << 5) | b5;
}

void LibRetroCamera::XRGB8888ToYUV422(u32 pixel1, u32 pixel2, u16& yuv1, u16& yuv2) {
    // Input format from avfoundation is BGRA
    // In memory: [B, G, R, A], as uint32_t on little-endian: 0xAARRGGBB
    // Extract RGB from first pixel
    int r1 = (pixel1 >> 16) & 0xFF;
    int g1 = (pixel1 >> 8) & 0xFF;
    int b1 = pixel1 & 0xFF;

    // Extract RGB from second pixel
    int r2 = (pixel2 >> 16) & 0xFF;
    int g2 = (pixel2 >> 8) & 0xFF;
    int b2 = pixel2 & 0xFF;

    // Convert to YUV
    int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;
    int y2 = ((66 * r2 + 129 * g2 + 25 * b2 + 128) >> 8) + 16;

    // Average U and V
    int u = ((-38 * r1 - 74 * g1 + 112 * b1 + 128) >> 8) + 128;
    u += ((-38 * r2 - 74 * g2 + 112 * b2 + 128) >> 8) + 128;
    u /= 2;

    int v = ((112 * r1 - 94 * g1 - 18 * b1 + 128) >> 8) + 128;
    v += ((112 * r2 - 94 * g2 - 18 * b2 + 128) >> 8) + 128;
    v /= 2;

    // Clamp values
    y1 = std::clamp(y1, 0, 255);
    y2 = std::clamp(y2, 0, 255);
    u = std::clamp(u, 0, 255);
    v = std::clamp(v, 0, 255);

    // Pack as YUYV (Y1 U Y2 V format, stored as two u16 values)
    yuv1 = (u8)y1 | ((u8)u << 8);
    yuv2 = (u8)y2 | ((u8)v << 8);
}

void LibRetroCamera::ApplyFlip(std::vector<u16>& frame, int width, int height) {
    if (flip_mode == Service::CAM::Flip::NoFlip) {
        return;
    }

    std::vector<u16> temp = frame;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int src_x = x;
            int src_y = y;

            switch (flip_mode) {
            case Service::CAM::Flip::Horizontal:
                src_x = width - 1 - x;
                break;
            case Service::CAM::Flip::Vertical:
                src_y = height - 1 - y;
                break;
            case Service::CAM::Flip::Reverse:
                src_x = width - 1 - x;
                src_y = height - 1 - y;
                break;
            default:
                break;
            }

            frame[y * width + x] = temp[src_y * width + src_x];
        }
    }
}

std::vector<u16> LibRetroCamera::ReceiveFrame() {
    auto& state = GetCameraState();

    // If no frame available or camera not initialized, return black frame
    if (!state.camera_initialized || output_width == 0 || output_height == 0) {
        LOG_DEBUG(Service_CAM, "LibRetro: ReceiveFrame - not initialized or zero size");
        return std::vector<u16>(output_width * output_height, output_rgb ? 0 : 0x8000);
    }

    std::vector<u32> source_frame;
    unsigned src_width, src_height;

    {
        std::lock_guard<std::mutex> lock(state.frame_mutex);
        if (!state.frame_available || state.frame_buffer.empty()) {
            LOG_DEBUG(Service_CAM, "LibRetro: ReceiveFrame - no frame available");
            return std::vector<u16>(output_width * output_height, output_rgb ? 0 : 0x8000);
        }

        source_frame = state.frame_buffer;
        src_width = state.frame_width;
        src_height = state.frame_height;
    }
    
    // Debug: Log frame processing (only occasionally)
    static int receive_count = 0;
    if (++receive_count % 60 == 0) {
        LOG_DEBUG(Service_CAM, "LibRetro: ReceiveFrame {} (src: {}x{}, out: {}x{})", 
                  receive_count, src_width, src_height, output_width, output_height);
    }

    std::vector<u16> result(output_width * output_height);

    // Simple nearest-neighbor scaling
    for (int y = 0; y < output_height; y++) {
        int src_y = y * src_height / output_height;
        src_y = std::min(src_y, (int)src_height - 1);

        for (int x = 0; x < output_width; x++) {
            int src_x = x * src_width / output_width;
            src_x = std::min(src_x, (int)src_width - 1);

            u32 pixel = source_frame[src_y * src_width + src_x];

            if (output_rgb) {
                result[y * output_width + x] = XRGB8888ToRGB565(pixel);
            } else {
                // For YUV422, we process pixels in pairs
                if (x % 2 == 0 && x + 1 < output_width) {
                    int src_x2 = (x + 1) * src_width / output_width;
                    src_x2 = std::min(src_x2, (int)src_width - 1);
                    u32 pixel2 = source_frame[src_y * src_width + src_x2];

                    u16 yuv1, yuv2;
                    XRGB8888ToYUV422(pixel, pixel2, yuv1, yuv2);
                    result[y * output_width + x] = yuv1;
                    result[y * output_width + x + 1] = yuv2;
                } else if (x % 2 == 1) {
                    // Already processed with previous pixel
                } else {
                    // Last odd pixel - use same pixel twice
                    u16 yuv1, yuv2;
                    XRGB8888ToYUV422(pixel, pixel, yuv1, yuv2);
                    result[y * output_width + x] = yuv1;
                }
            }
        }
    }

    ApplyFlip(result, output_width, output_height);

    return result;
}

bool LibRetroCamera::IsPreviewAvailable() {
    auto& state = GetCameraState();
    return state.camera_initialized && state.frame_available;
}

} // namespace LibRetro::Camera

