// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include "common/common_types.h"
#include "core/frontend/camera/interface.h"
#include "core/hle/service/cam/cam.h"
#include "libretro.h"

namespace LibRetro::Camera {

// Default camera dimensions (3DS inner camera resolution)
constexpr unsigned DEFAULT_CAMERA_WIDTH = 640;
constexpr unsigned DEFAULT_CAMERA_HEIGHT = 480;

/**
 * A camera implementation that receives frames from the libretro frontend's camera driver.
 */
class LibRetroCamera final : public ::Camera::CameraInterface {
public:
    /// @param flip The flip mode for the camera
    /// @param camera_id The 3DS camera ID (0=OuterRight, 1=Inner, 2=OuterLeft)
    LibRetroCamera(const Service::CAM::Flip& flip, int camera_id = -1);
    ~LibRetroCamera() override;

    void StartCapture() override;
    void StopCapture() override;
    void SetResolution(const Service::CAM::Resolution& resolution) override;
    void SetFlip(Service::CAM::Flip flip) override;
    void SetEffect(Service::CAM::Effect effect) override;
    void SetFormat(Service::CAM::OutputFormat format) override;
    void SetFrameRate(Service::CAM::FrameRate frame_rate) override;
    std::vector<u16> ReceiveFrame() override;
    bool IsPreviewAvailable() override;

    /// Called by the libretro callback when a new camera frame is received
    static void OnCameraFrame(const uint32_t* buffer, unsigned width, unsigned height, size_t pitch);

private:
    /// Converts XRGB8888 to RGB565
    static u16 XRGB8888ToRGB565(u32 pixel);

    /// Converts XRGB8888 to YUV422
    static void XRGB8888ToYUV422(u32 pixel1, u32 pixel2, u16& yuv1, u16& yuv2);

    /// Applies flip transformation to the frame
    void ApplyFlip(std::vector<u16>& frame, int width, int height);

    int output_width = 0;
    int output_height = 0;
    bool output_rgb = false;
    Service::CAM::Flip flip_mode = Service::CAM::Flip::NoFlip;
    bool capturing = false;
    int camera_id = -1;  // 3DS camera ID: 0=OuterRight, 1=Inner, 2=OuterLeft
};

/// Global camera state management
struct CameraState {
    std::mutex frame_mutex;
    std::vector<u32> frame_buffer;
    unsigned frame_width = 0;
    unsigned frame_height = 0;
    bool frame_available = false;
    bool camera_initialized = false;
    retro_camera_callback callback = {};
};

/// Get the global camera state
CameraState& GetCameraState();

/// Initialize the libretro camera interface
bool InitializeCameraInterface();

/// Shutdown the libretro camera interface
void ShutdownCameraInterface();

} // namespace LibRetro::Camera

