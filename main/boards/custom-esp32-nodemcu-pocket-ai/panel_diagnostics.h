#pragma once
#include <lvgl.h>
#include <cstdint>

namespace pocket {
enum class PanelPattern : uint8_t {
    Black,
    White,
    Red,
    Green,
    Blue,
    Checkerboard,
    Horizontal,
    Vertical,
    ClearWhite,
    ClearBlack,
    Count
};
constexpr unsigned kPanelTestStages = static_cast<unsigned>(PanelPattern::Count);
constexpr uint64_t kPanelTestHoldMs = 3000;
const char* PanelPatternName(PanelPattern pattern);
// One opaque overlay, drawn through the existing 11200-byte partial strip.
// No caption, underlying face animation, canvas or additional framebuffer.
class PanelDiagnostics {
public:
    explicit PanelDiagnostics(lv_obj_t* screen);
    ~PanelDiagnostics();
    void Show(PanelPattern pattern);
    void Hide();

private:
    lv_obj_t* root_ = nullptr;
    PanelPattern pattern_ = PanelPattern::Black;
    static void Draw(lv_event_t* event);
};
}  // namespace pocket
