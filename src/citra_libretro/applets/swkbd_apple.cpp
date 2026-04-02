// Copyright 2025 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <thread>
#include "common/logging/log.h"
#include "citra_libretro/applets/swkbd_apple.h"

namespace SoftwareKeyboard {

// Static member initialization
KeyboardRequestCallback AppleKeyboard::s_keyboard_request_callback = nullptr;
AppleKeyboard* AppleKeyboard::s_current_instance = nullptr;

AppleKeyboard::AppleKeyboard() = default;

AppleKeyboard::~AppleKeyboard() {
    if (s_current_instance == this) {
        s_current_instance = nullptr;
    }
}

void AppleKeyboard::SetKeyboardRequestCallback(KeyboardRequestCallback callback) {
    s_keyboard_request_callback = callback;
}

void AppleKeyboard::Execute(const Frontend::KeyboardConfig& config_) {
    SoftwareKeyboard::Execute(config_);
    
    s_current_instance = this;
    
    // If no callback is registered, use default behavior
    if (!s_keyboard_request_callback) {
        LOG_WARNING(Frontend, "Apple keyboard callback not registered, using empty input");
        Finalize("", 0);
        return;
    }
    
    // Store strings for the lifetime of the config
    stored_hint_text = config.hint_text;
    stored_button_texts.clear();
    stored_button_text_ptrs.clear();
    
    for (const auto& text : config.button_text) {
        stored_button_texts.push_back(text);
    }
    for (const auto& text : stored_button_texts) {
        stored_button_text_ptrs.push_back(text.c_str());
    }
    
    // Build the C-compatible config structure
    AppleKeyboardConfig apple_config{};
    apple_config.button_config = static_cast<int>(config.button_config);
    apple_config.accept_mode = static_cast<int>(config.accept_mode);
    apple_config.multiline_mode = config.multiline_mode;
    apple_config.max_text_length = config.max_text_length;
    apple_config.max_digits = config.max_digits;
    apple_config.hint_text = stored_hint_text.c_str();
    apple_config.button_text = stored_button_text_ptrs.data();
    apple_config.button_text_count = static_cast<int>(stored_button_text_ptrs.size());
    apple_config.prevent_digit = config.filters.prevent_digit;
    apple_config.prevent_at = config.filters.prevent_at;
    apple_config.prevent_percent = config.filters.prevent_percent;
    apple_config.prevent_backslash = config.filters.prevent_backslash;
    apple_config.prevent_profanity = config.filters.prevent_profanity;
    apple_config.enable_callback = config.filters.enable_callback;
    
    LOG_INFO(Frontend, "Requesting Apple keyboard input, hint: {}", config.hint_text);
    
    // Call the callback to request keyboard from frontend
    s_keyboard_request_callback(apple_config);
}

void AppleKeyboard::ShowError(const std::string& error) {
    LOG_ERROR(Frontend, "Apple keyboard error: {}", error);
}

void AppleKeyboard::SubmitInput(const std::string& text, u8 button) {
    Finalize(text, button);
}

} // namespace SoftwareKeyboard

// C API for Libretro frontend to submit keyboard input
extern "C" {

void retro_apple_keyboard_submit(const char* text, int button) {
    if (SoftwareKeyboard::AppleKeyboard::s_current_instance) {
        std::string text_str = text ? text : "";
        SoftwareKeyboard::AppleKeyboard::s_current_instance->SubmitInput(text_str, static_cast<u8>(button));
    }
}

}

