#pragma once
#include <driver/gpio.h>

// One clock domain. The upstream audio service resamples negotiated downlink.
#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000
#define AUDIO_BCLK GPIO_NUM_26
#define AUDIO_WS GPIO_NUM_25
#define AUDIO_MIC_DIN GPIO_NUM_34
#define AUDIO_SPEAKER_DOUT GPIO_NUM_22
#define DISPLAY_SCK GPIO_NUM_18
#define DISPLAY_MOSI GPIO_NUM_23
#define DISPLAY_CS GPIO_NUM_13
#define DISPLAY_DC GPIO_NUM_27
#define DISPLAY_RST GPIO_NUM_14
// Set to 1 for the opposite landscape orientation. GPIOs stay identical.
#define POCKET_LANDSCAPE_FLIPPED 0
#define DISPLAY_WIDTH 280
#define DISPLAY_HEIGHT 240
// The 240x280 glass is centered within ST7789's 240x320 RAM.
// After exchanging axes its 20-pixel inset moves from Y to X.
#define DISPLAY_OFFSET_X 20
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_MIRROR_X (!POCKET_LANDSCAPE_FLIPPED)
#define DISPLAY_MIRROR_Y (POCKET_LANDSCAPE_FLIPPED)
#define DISPLAY_SWAP_XY true
#define DISPLAY_BGR false
#define DISPLAY_INVERT true
#define POCKET_PTT_GPIO GPIO_NUM_0
// Optional external button: change POCKET_PTT_GPIO to GPIO_NUM_32.
// BLK is wired to 3V3. No backlight, battery, I2C codec, or touch GPIO.
