#include "panel_diagnostics.h"
#include <algorithm>

namespace pocket {
namespace {
constexpr int kWidth = 280, kHeight = 240;
uint32_t Background(PanelPattern pattern) {
    switch (pattern) {
        case PanelPattern::White:
        case PanelPattern::ClearWhite:
            return 0xffffff;
        case PanelPattern::Red:
            return 0xff0000;
        case PanelPattern::Green:
            return 0x00ff00;
        case PanelPattern::Blue:
            return 0x0000ff;
        default:
            return 0;
    }
}
void Rectangle(lv_layer_t* layer, const lv_area_t& area) {
    if (area.x1 > layer->_clip_area.x2 || area.x2 < layer->_clip_area.x1 ||
        area.y1 > layer->_clip_area.y2 || area.y2 < layer->_clip_area.y1)
        return;
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = lv_color_white();
    lv_draw_rect(layer, &rect, &area);
}
}  // namespace
const char* PanelPatternName(PanelPattern pattern) {
    switch (pattern) {
        case PanelPattern::Black:
            return "solid BLACK: inspect faint lines/patches";
        case PanelPattern::White:
            return "solid WHITE: inspect dark lines/stuck pixels";
        case PanelPattern::Red:
            return "solid RED: inspect color order/uniformity";
        case PanelPattern::Green:
            return "solid GREEN: inspect uniformity";
        case PanelPattern::Blue:
            return "solid BLUE: inspect color order/uniformity";
        case PanelPattern::Checkerboard:
            return "16px CHECKERBOARD: inspect edges/coordinates";
        case PanelPattern::Horizontal:
            return "HORIZONTAL 1px lines every 12px";
        case PanelPattern::Vertical:
            return "VERTICAL 1px lines every 12px";
        case PanelPattern::ClearWhite:
            return "CLEAR prepare: full-screen WHITE overwrite";
        case PanelPattern::ClearBlack:
            return "CLEAR verify: full-screen BLACK; no previous pattern";
        default:
            return "invalid pattern";
    }
}
PanelDiagnostics::PanelDiagnostics(lv_obj_t* screen) {
    root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(root_);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, kWidth, kHeight);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_add_event_cb(root_, Draw, LV_EVENT_DRAW_MAIN, this);
    Hide();
}
PanelDiagnostics::~PanelDiagnostics() { lv_obj_delete(root_); }
void PanelDiagnostics::Show(PanelPattern pattern) {
    if (pattern >= PanelPattern::Count)
        return;
    const bool hidden = lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN);
    if (!hidden && pattern == pattern_)
        return;
    pattern_ = pattern;
    lv_obj_set_style_bg_color(root_, lv_color_hex(Background(pattern)), 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    // Pattern changes with the same background also require a complete clear.
    lv_obj_invalidate(root_);
}
void PanelDiagnostics::Hide() { lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN); }
void PanelDiagnostics::Draw(lv_event_t* event) {
    auto* self = static_cast<PanelDiagnostics*>(lv_event_get_user_data(event));
    auto* layer = lv_event_get_layer(event);
    lv_area_t bounds;
    lv_obj_get_coords(self->root_, &bounds);
    if (self->pattern_ == PanelPattern::Checkerboard) {
        for (int y = 0; y < kHeight; y += 16)
            for (int x = 0; x < kWidth; x += 16)
                if ((x / 16 + y / 16) % 2 == 0)
                    Rectangle(layer, {bounds.x1 + x, bounds.y1 + y,
                                      bounds.x1 + std::min(x + 15, kWidth - 1),
                                      bounds.y1 + std::min(y + 15, kHeight - 1)});
    } else if (self->pattern_ == PanelPattern::Horizontal) {
        for (int y = 0; y < kHeight; y += 12)
            Rectangle(layer, {bounds.x1, bounds.y1 + y, bounds.x2, bounds.y1 + y});
    } else if (self->pattern_ == PanelPattern::Vertical) {
        for (int x = 0; x < kWidth; x += 12)
            Rectangle(layer, {bounds.x1 + x, bounds.y1, bounds.x1 + x, bounds.y2});
    }
}
}  // namespace pocket
