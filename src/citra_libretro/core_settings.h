// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <string>
#include "core/hle/service/cfg/cfg.h"

namespace LibRetro {

enum CStickFunction { Both, CStick, Touchscreen };

struct CoreSettings {

    std::string file_path;

    float analog_deadzone = 1.f;

    LibRetro::CStickFunction analog_function;

    bool enable_mouse_touchscreen;

    Service::CFG::SystemLanguage language_value;

    bool enable_touch_touchscreen;

    bool enable_touch_pointer_timeout;

    std::string swap_screen_mode;

    bool enable_motion;

    float motion_sensitivity;

    /// Display rotation for iOS motion mapping.
    /// 0=portrait, 1=landscapeLeft, 2=portraitUpsideDown, 3=landscapeRight.
    int motion_rotation = 3;

    // Custom layout buffer dimensions (0 means auto-calculate)
    unsigned custom_layout_buffer_width = 0;
    unsigned custom_layout_buffer_height = 0;

} extern settings;

void RegisterCoreOptions(void);
void ParseCoreOptions(void);

} // namespace LibRetro
