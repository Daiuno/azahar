// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_libretro/camera/libretro_camera_factory.h"
#include "common/logging/log.h"
#include "core/frontend/camera/blank_camera.h"

namespace LibRetro::Camera {

std::unique_ptr<::Camera::CameraInterface> Factory::Create(const std::string& config,
                                                           const Service::CAM::Flip& flip) {
    // For now, return a blank camera that provides black frames
    // This prevents games from failing when camera functionality is requested
    LOG_INFO(Service_CAM, "LibRetro: Creating blank camera (config: {})", config);
    return std::make_unique<::Camera::BlankCamera>();
}

void RegisterCameraFactory() {
    // Register a blank camera factory as the default
    // This ensures games that use the camera don't crash or hang
    ::Camera::RegisterFactory("blank", std::make_unique<Factory>());

    // Also register as "image" for compatibility
    ::Camera::RegisterFactory("image", std::make_unique<Factory>());

    LOG_INFO(Service_CAM, "LibRetro: Camera factory registered");
}

} // namespace LibRetro::Camera
