#include "clock_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

LV_FONT_DECLARE(pocket_clock_digits_64);
LV_FONT_DECLARE(font_noto_sans_basic_16_4);
namespace pocket {
namespace {
constexpr int kWidth = 280, kHeight = 240;
constexpr uint32_t kNavy = 0x020516;
uint32_t Mix(uint32_t a, uint32_t b, unsigned fraction) {
    uint32_t result = 0;
    for (int shift : {0, 8, 16})
        result |=
            ((((a >> shift) & 255) * (255 - fraction) + ((b >> shift) & 255) * fraction) / 255)
            << shift;
    return result;
}
bool Intersects(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}
lv_obj_t* Object(lv_obj_t* parent, int x, int y, int width, int height) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    return obj;
}
}  // namespace
ClockRenderer::ClockRenderer(lv_obj_t* screen) {
    root_ = Object(screen, 0, 0, kWidth, kHeight);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kNavy), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(root_, Background, LV_EVENT_DRAW_MAIN, this);
    digits_ = Object(root_, 8, 77, 264, 78);
    lv_obj_add_event_cb(digits_, Digits, LV_EVENT_DRAW_MAIN, this);
    date_ = lv_label_create(root_);
    lv_obj_remove_style_all(date_);
    lv_obj_set_pos(date_, 10, 164);
    lv_obj_set_size(date_, 260, 25);
    lv_obj_set_style_text_font(date_, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(date_, lv_color_hex(0xe7e9f6), 0);
    lv_obj_set_style_text_align(date_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(date_, LV_LABEL_LONG_CLIP);
    lv_label_set_text_static(date_, text_.date);
    Visible(false);
}
ClockRenderer::~ClockRenderer() { lv_obj_delete(root_); }
void ClockRenderer::Visible(bool visible) {
    if (visible)
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}
void ClockRenderer::Update(const ClockText& text) {
    const bool digits_changed = !initialized_ || text.valid != text_.valid ||
                                std::strcmp(text.time, text_.time) ||
                                std::strcmp(text.period, text_.period);
    const bool date_changed = !initialized_ || std::strcmp(text.date, text_.date);
    text_ = text;
    initialized_ = true;
    if (digits_changed)
        lv_obj_invalidate(digits_);
    if (date_changed) {
        lv_label_set_text_static(date_, text_.date);
        lv_obj_invalidate(date_);
    }
}
void ClockRenderer::Background(lv_event_t* event) {
    auto* layer = lv_event_get_layer(event);
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
    // Eight-pixel horizontal bands with two smooth horizontal gradients.
    // Only bands intersecting this partial strip become draw tasks.
    for (int y = 0; y < kHeight; y += 8) {
        const float dy = (y + 4 - 119) / 82.0f;
        const unsigned strength = static_cast<unsigned>(150 * std::exp(-dy * dy));
        for (int side = 0; side < 2; ++side) {
            lv_area_t area = {bounds.x1 + side * 140, bounds.y1 + y, bounds.x1 + side * 140 + 139,
                              bounds.y1 + y + 7};
            if (!Intersects(area, layer->_clip_area))
                continue;
            lv_draw_rect_dsc_t rect;
            lv_draw_rect_dsc_init(&rect);
            const auto glow = lv_color_hex(Mix(kNavy, side ? 0x6507a0 : 0x00599a, strength));
            rect.bg_color = side ? lv_color_hex(kNavy) : glow;
            rect.bg_grad.dir = LV_GRAD_DIR_HOR;
            rect.bg_grad.stops_count = 2;
            rect.bg_grad.stops[0] = {rect.bg_color, LV_OPA_COVER, 0};
            rect.bg_grad.stops[1] = {side ? glow : lv_color_hex(kNavy), LV_OPA_COVER, 255};
            lv_draw_rect(layer, &rect, &area);
        }
    }
    // Quiet arcs, clipped at the edges, behind the digits.
    for (int side = 0; side < 2; ++side) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.center = {bounds.x1 + 140, bounds.y1 + 122};
        arc.radius = 125;
        arc.width = 2;
        arc.color = lv_color_hex(side ? 0x3a0d61 : 0x083259);
        arc.start_angle = side ? 285 : 155;
        arc.end_angle = side ? 345 : 215;
        lv_draw_arc(layer, &arc);
    }
}
void ClockRenderer::Digits(lv_event_t* event) {
    auto* self = static_cast<ClockRenderer*>(lv_event_get_user_data(event));
    auto* layer = lv_event_get_layer(event);
    lv_area_t bounds;
    lv_obj_get_coords(self->digits_, &bounds);
    lv_point_t size;
    lv_text_get_size(&size, self->text_.time, &pocket_clock_digits_64, 0, 0, 260,
                     LV_TEXT_FLAG_NONE);
    const int total = size.x + (self->text_.valid ? 35 : 0);
    lv_area_t text_area = {bounds.x1 + (264 - total) / 2, bounds.y1,
                           bounds.x1 + (264 - total) / 2 + size.x - 1, bounds.y2};
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &pocket_clock_digits_64;
    label.text = self->text_.time;
    label.text_static = 1;
    // LVGL explicitly snapshots the layer clipping area when a draw task is
    // created. Restore it before returning. A small band count bounds tasks/RAM;
    // this colors the actual anti-aliased glyphs, without a text-sized canvas.
    const lv_area_t saved = layer->_clip_area;
    for (int x = 0; x < size.x; x += 8) {
        lv_area_t band = {text_area.x1 + x, text_area.y1,
                          std::min(text_area.x1 + x + 7, text_area.x2), text_area.y2};
        if (!Intersects(band, saved))
            continue;
        layer->_clip_area = {std::max(saved.x1, band.x1), std::max(saved.y1, band.y1),
                             std::min(saved.x2, band.x2), std::min(saved.y2, band.y2)};
        const unsigned t = std::min<unsigned>(255, (x + 4) * 255 / std::max<int32_t>(1, size.x));
        label.color = lv_color_hex(t < 128 ? Mix(0x9af5ff, 0x9278ff, t * 2)
                                           : Mix(0x9278ff, 0xf45aff, (t - 128) * 2));
        lv_draw_label(layer, &label, &text_area);
    }
    layer->_clip_area = saved;
    if (self->text_.valid) {
        label.font = &font_noto_sans_basic_16_4;
        label.text = self->text_.period;
        label.color = lv_color_hex(0xf3eaff);
        const lv_area_t period = {text_area.x2 + 8, bounds.y1 + 43, bounds.x2, bounds.y2};
        lv_draw_label(layer, &label, &period);
    }
}
}  // namespace pocket
