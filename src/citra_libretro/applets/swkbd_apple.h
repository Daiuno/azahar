// Copyright 2025 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <string>
#include "core/frontend/applets/swkbd.h"

namespace SoftwareKeyboard {

// C-compatible keyboard config structure for Libretro API
struct AppleKeyboardConfig {
    int button_config;      // ButtonConfig enum value (0=Single, 1=Dual, 2=Triple, 3=None)
    int accept_mode;        // AcceptedInput enum value
    bool multiline_mode;
    int max_text_length;
    int max_digits;
    const char* hint_text;
    const char** button_text;
    int button_text_count;
    // Filters
    bool prevent_digit;
    bool prevent_at;
    bool prevent_percent;
    bool prevent_backslash;
    bool prevent_profanity;
    bool enable_callback;
};

// Callback type for keyboard request
using KeyboardRequestCallback = std::function<void(const AppleKeyboardConfig& config)>;

class AppleKeyboard final : public Frontend::SoftwareKeyboard {
public:
    AppleKeyboard();
    ~AppleKeyboard();

    void Execute(const Frontend::KeyboardConfig& config) override;
    void ShowError(const std::string& error) override;

    // Called by frontend to submit keyboard result
    void SubmitInput(const std::string& text, u8 button);

    // Set the callback that will be called when keyboard is needed
    static void SetKeyboardRequestCallback(KeyboardRequestCallback callback);

    // Allow C API to access current instance
    static AppleKeyboard* s_current_instance;

private:
    static KeyboardRequestCallback s_keyboard_request_callback;
    
    std::string pending_text;
    u8 pending_button = 0;
    
    // Storage for C-string pointers
    std::string stored_hint_text;
    std::vector<std::string> stored_button_texts;
    std::vector<const char*> stored_button_text_ptrs;
};

} // namespace SoftwareKeyboard

