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
 * A simple camera factory for LibRetro that provides a basic still image camera.
 * This allows games that require camera functionality to work properly.
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
