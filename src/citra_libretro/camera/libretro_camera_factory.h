// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include "core/frontend/camera/factory.h"
#include "core/frontend/camera/interface.h"

namespace LibRetro::Camera {

/**
 * A camera factory for LibRetro that uses the frontend's camera driver.
 * Falls back to blank camera if the frontend doesn't support camera.
 */
class Factory final : public ::Camera::CameraFactory {
public:
    std::unique_ptr<::Camera::CameraInterface> Create(const std::string& config,
                                                      const Service::CAM::Flip& flip) override;
};

/**
 * Registers the LibRetro camera factory.
 * This should be called during core initialization.
 */
void RegisterCameraFactory();

} // namespace LibRetro::Camera
