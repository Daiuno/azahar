// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_libretro/camera/libretro_camera.h"
#include "citra_libretro/camera/libretro_camera_factory.h"
#include "common/logging/log.h"
#include "core/frontend/camera/blank_camera.h"

namespace LibRetro::Camera {

std::unique_ptr<::Camera::CameraInterface> Factory::Create(const std::string& config,
                                                           const Service::CAM::Flip& flip) {
    auto& state = GetCameraState();

    // Parse camera ID from config string
    int camera_id = -1;
    if (!config.empty()) {
        try {
            camera_id = std::stoi(config);
        } catch (...) {
            LOG_WARNING(Service_CAM, "LibRetro: Failed to parse camera ID from config: {}", config);
        }
    }

    if (state.camera_initialized) {
        LOG_INFO(Service_CAM, "LibRetro: Creating LibRetroCamera (config: {}, camera_id: {})", config, camera_id);
        return std::make_unique<LibRetroCamera>(flip, camera_id);
    }

    // Fallback to blank camera if libretro camera interface is not available
    LOG_INFO(Service_CAM, "LibRetro: Camera interface not available, using blank camera (config: {})", config);
    return std::make_unique<::Camera::BlankCamera>();
}

void RegisterCameraFactory() {
    // Register the libretro camera factory as the default for various camera types
    ::Camera::RegisterFactory("blank", std::make_unique<Factory>());
    ::Camera::RegisterFactory("image", std::make_unique<Factory>());

    LOG_INFO(Service_CAM, "LibRetro: Camera factory registered");
}

} // namespace LibRetro::Camera
