#pragma once
#include <lvgl.h>
#include "clock_state.h"

namespace pocket {
// Three objects, static glyphs in flash, and the existing partial RGB565 strip.
// No canvas, image cache, framebuffer, or per-second full-screen invalidation.
class ClockRenderer {
public:
    explicit ClockRenderer(lv_obj_t* screen);
    ~ClockRenderer();
    ClockRenderer(const ClockRenderer&) = delete;
    ClockRenderer& operator=(const ClockRenderer&) = delete;
    void Visible(bool visible);
    void Update(const ClockText& text);

private:
    lv_obj_t *root_ = nullptr, *digits_ = nullptr, *date_ = nullptr;
    ClockText text_{};
    bool initialized_ = false;
    static void Background(lv_event_t* event);
    static void Digits(lv_event_t* event);
};
}  // namespace pocket
