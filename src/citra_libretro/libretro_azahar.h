/* Copyright Citra Emulator Project / Azahar Emulator Project
 * Licensed under GPLv2 or any later version
 * Refer to the license.txt file included.
 *
 * Azahar libretro core — 3DS-specific extension API.
 *
 * Plain-C header consumed by a custom RetroArch / ManicEmu frontend that loads
 * the azahar.libretro core. Symbols use the "retro_azahar_" prefix so they never
 * collide with vanilla libretro.h from the upstream libretro-common submodule:
 *   https://github.com/libretro/libretro-common.git
 *
 * Standard libretro entry points (retro_init, retro_run, …) remain in libretro.h.
 * Include this header only when you need Azahar-specific features (Amiibo, SW
 * keyboard, CIA install).
 *
 * Threading: call from the same thread that invokes retro_run (libretro contract).
 */

#ifndef LIBRETRO_AZAHAR_H
#define LIBRETRO_AZAHAR_H

#include <stdbool.h>
#include <stddef.h>

#ifndef LIBRETRO_AZAHAR_API
#  if defined(_WIN32) && !defined(__GNUC__)
#    define LIBRETRO_AZAHAR_API __declspec(dllexport)
#  else
#    define LIBRETRO_AZAHAR_API __attribute__((visibility("default")))
#  endif
#endif

#ifndef RETRO_AZAHAR_CALLCONV
#  if defined(__GNUC__) && defined(__i386__) && !defined(__x86_64__)
#    define RETRO_AZAHAR_CALLCONV __attribute__((cdecl))
#  elif defined(_MSC_VER) && defined(_M_IX86) && !defined(_M_X64)
#    define RETRO_AZAHAR_CALLCONV __cdecl
#  else
#    define RETRO_AZAHAR_CALLCONV
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when the extension ABI changes. */
#define RETRO_AZAHAR_EXTENSION_VERSION 1

LIBRETRO_AZAHAR_API unsigned retro_azahar_extension_version(void);

/* -------------------------------------------------------------------------- */
/* Amiibo (NFC)                                                               */
/* -------------------------------------------------------------------------- */

LIBRETRO_AZAHAR_API bool retro_azahar_load_amiibo(const char* path);
LIBRETRO_AZAHAR_API bool retro_azahar_is_searching_amiibo(void);
LIBRETRO_AZAHAR_API void retro_azahar_remove_amiibo(void);

/* -------------------------------------------------------------------------- */
/* Software keyboard (Apple / frontend-driven SWKBD)                          */
/* -------------------------------------------------------------------------- */

struct retro_azahar_keyboard_config {
    int button_config; /* 0=Single, 1=Dual, 2=Triple, 3=None */
    int accept_mode;   /* 0=Anything, 1=NotEmpty, 2=NotEmptyAndNotBlank, 3=NotBlank, 4=FixedLength */
    bool multiline_mode;
    int max_text_length;
    int max_digits;
    const char* hint_text;
    const char** button_text;
    int button_text_count;
    bool prevent_digit;
    bool prevent_at;
    bool prevent_percent;
    bool prevent_backslash;
    bool prevent_profanity;
    bool enable_callback;
};

typedef void(RETRO_AZAHAR_CALLCONV* retro_azahar_keyboard_callback_t)(
    const struct retro_azahar_keyboard_config* config);

LIBRETRO_AZAHAR_API void retro_azahar_set_keyboard_callback(
    retro_azahar_keyboard_callback_t callback);

/**
 * @param text   User input, or NULL if cancelled.
 * @param button 0=Ok (Single); Dual: 0=Cancel, 1=Ok; Triple: 0=Cancel, 1=Forgot, 2=Ok.
 */
LIBRETRO_AZAHAR_API void retro_azahar_keyboard_input(const char* text, int button);

/* -------------------------------------------------------------------------- */
/* Title install                                                              */
/* -------------------------------------------------------------------------- */

LIBRETRO_AZAHAR_API void retro_azahar_install_cia(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_AZAHAR_H */
