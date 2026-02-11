// ============================================================
// User_Setup.h — TFT_eSPI config for LilyGO T-Display S3
// ST7789 170x320, 8-bit parallel interface
//
// NOTE: These settings are also applied via build_flags in
//       platformio.ini. This file serves as documentation
//       and for Arduino IDE compatibility.
// ============================================================

#ifndef USER_SETUP_H
#define USER_SETUP_H

#define USER_SETUP_ID 206

#define ST7789_DRIVER
#define INIT_SEQUENCE_3
#define CGRAM_OFFSET
#define TFT_RGB_ORDER TFT_RGB
#define TFT_INVERSION_ON

#define TFT_PARALLEL_8_BIT

#define TFT_WIDTH   170
#define TFT_HEIGHT  320

// --- 8-bit parallel data bus ---
#define TFT_D0      39
#define TFT_D1      40
#define TFT_D2      41
#define TFT_D3      42
#define TFT_D4      45
#define TFT_D5      46
#define TFT_D6      47
#define TFT_D7      48

// --- Control pins ---
#define TFT_CS       6
#define TFT_DC       7
#define TFT_RST      5
#define TFT_WR       8
#define TFT_RD       9

// --- Backlight ---
#define TFT_BL      38
#define TFT_BACKLIGHT_ON HIGH

// --- Fonts ---
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#endif
