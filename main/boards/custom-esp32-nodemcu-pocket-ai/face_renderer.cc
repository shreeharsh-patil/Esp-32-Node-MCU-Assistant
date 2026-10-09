#include "face_renderer.h"
#include <cmath>

namespace pocket::face {
namespace {
lv_obj_t* Shape(lv_obj_t* parent, uint32_t color, int radius = LV_RADIUS_CIRCLE) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}
void Bounds(lv_obj_t* obj, const Rect& rect) {
    // LVGL setters skip invalidation for unchanged integer geometry.
    lv_obj_set_pos(obj, rect.x, rect.y);
    lv_obj_set_size(obj, rect.w, rect.h);
}
void Opacity(lv_obj_t* obj, uint8_t value) {
    if (lv_obj_get_style_opa(obj, LV_PART_MAIN) != value)
        lv_obj_set_style_opa(obj, value, 0);
}
Rect Expand(const Rect& rect, int amount) {
    return {rect.x - amount, rect.y - amount, rect.w + 2 * amount, rect.h + 2 * amount};
}
uint32_t Shade(uint32_t color, uint32_t brightness) {
    return (((color >> 16) & 255) * brightness / 255 << 16) |
           (((color >> 8) & 255) * brightness / 255 << 8) | ((color & 255) * brightness / 255);
}
lv_obj_t* Line(lv_obj_t* parent, int width) {
    auto* line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_style_line_color(line, lv_color_hex(kEye), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}
void Curve(lv_obj_t* line, std::array<lv_point_precise_t, 9>& points, int width, int bend) {
    bool changed = false;
    for (int i = 0; i < 9; ++i) {
        lv_point_precise_t point = {static_cast<lv_value_precise_t>(i * width / 8),
                                    static_cast<lv_value_precise_t>(
                                        std::lround(10 + bend * std::sin(i * 3.14159265f / 8)))};
        if (points[i].x != point.x || points[i].y != point.y)
            changed = true;
        points[i] = point;
    }
    if (changed)
        lv_line_set_points(line, points.data(), points.size());
}
}  // namespace

Renderer::Renderer(lv_obj_t* screen) {
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(root_);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(root_, kWidth, kHeight);
    for (int i = 0; i < 2; ++i) {
        rim_[i] = Shape(root_, 0x000000, 43 - i * 3);
    }
    plate_ = Shape(root_, 0x000000, 38);
    for (auto& eye : eyes_) {
        for (auto& glow : eye.glow)
            glow = Shape(root_, kEye);
        eye.body = Shape(root_, kEye);
        lv_obj_set_style_bg_grad_color(eye.body, lv_color_hex(0xe1e5ef), 0);
        lv_obj_set_style_bg_grad_dir(eye.body, LV_GRAD_DIR_VER, 0);
        eye.highlight = Shape(root_, kHighlight);
        eye.sparkle = Shape(root_, 0xffffff);
        eye.smile = Line(root_, 7);
    }
    for (auto& cheek : cheeks_) {
        for (auto*& layer : cheek)
            layer = Shape(root_, kBlush);
    }
    smile_ = Line(root_, 4);
    mouth_ = Shape(root_, 0x000000);
    lv_obj_set_style_border_color(mouth_, lv_color_hex(kEye), 0);
    lv_obj_set_style_border_width(mouth_, 3, 0);
    for (auto& dot : dots_)
        dot = Shape(root_, kCoral);
    for (int i = 0; i < 3; ++i) {
        colors_[i] = Shape(screen, i == 0 ? 0xff0000 : i == 1 ? 0x00ff00 : 0x0000ff, 6);
        Bounds(colors_[i], {8 + i * 89, 8, 86, 180});
        lv_obj_add_flag(colors_[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void Renderer::Draw(const Frame& frame) {
    if (color_test_)
        return;
    Opacity(plate_, frame.plate_opacity);
    for (auto* rim : rim_)
        Opacity(rim, frame.plate_opacity);
    if (!drawn_ || !(frame.plate == previous_.plate)) {
        Bounds(plate_, frame.plate);
        for (int i = 0; i < 2; ++i)
            Bounds(rim_[i], Expand(frame.plate, i ? 2 : 5));
    }
    for (int i = 0; i < 2; ++i) {
        auto& eye = eyes_[i];
        const auto& rect = frame.eyes[i];
        for (int j = 0; j < 2; ++j) {
            Bounds(eye.glow[j], Expand(rect, j ? 3 : 7));
            Opacity(eye.glow[j], frame.glow * frame.opacity / (j ? 255 : 510));
        }
        Bounds(eye.body, rect);
        if (!drawn_ || frame.eye_color != previous_.eye_color)
            lv_obj_set_style_bg_color(eye.body, lv_color_hex(frame.eye_color), 0);
        const int normal_opa = frame.opacity * (255 - frame.happy_eyes) / 255;
        Opacity(eye.body, normal_opa);
        Bounds(eye.highlight,
               {rect.x + 7, rect.y + 6, std::max(1, rect.w - 14), std::max(1, rect.h / 3)});
        Bounds(eye.sparkle, {rect.x + 10, rect.y + 9, 6, 4});
        Opacity(eye.highlight, rect.h > 20 ? normal_opa * 150 / 255 : 0);
        Opacity(eye.sparkle, rect.h > 22 ? normal_opa * 190 / 255 : 0);
        Curve(eye.smile, eye.points, rect.w, -9);
        lv_obj_set_pos(eye.smile, rect.x, rect.y + rect.h / 2 - 10);
        Opacity(eye.smile, frame.happy_eyes * frame.opacity / 255);
        // Preblend five oval shades against our black background. Repeated very
        // low-alpha RGB565 mixing otherwise shifts dim pink toward green.
        // All objects are allocated once; LVGL invalidates their old/new bounds.
        for (int layer = 0; layer < 5; ++layer) {
            const auto& cheek = frame.cheeks[i];
            const int width = cheek.w * (100 - layer * 18) / 100;
            const int height = std::max(6, cheek.h * (100 - layer * 18) / 100);
            Bounds(cheeks_[i][layer], {cheek.x + (cheek.w - width) / 2,
                                       cheek.y + (cheek.h - height) / 2, width, height});
            if (!drawn_ || frame.blush != previous_.blush || frame.opacity != previous_.opacity) {
                const uint32_t brightness =
                    (35 + layer * 33) * frame.blush * frame.opacity / (128 * 255);
                lv_obj_set_style_bg_color(cheeks_[i][layer],
                                          lv_color_hex(Shade(kBlush, brightness)), 0);
            }
        }
    }
    Curve(smile_, smile_points_, frame.smile_width, frame.smile_curve);
    lv_obj_set_pos(smile_, 140 + frame.head_x - frame.smile_width / 2, 147 + frame.head_y);
    Opacity(smile_, (255 - frame.mouth_opacity) * frame.opacity / 255);
    Bounds(mouth_, frame.mouth);
    Opacity(mouth_, frame.mouth_opacity * frame.opacity / 255);
    for (int i = 0; i < 3; ++i) {
        Bounds(dots_[i], {127 + i * 10, std::min(173, frame.plate.y + frame.plate.h - 17), 5, 5});
        Opacity(dots_[i], frame.dots[i]);
    }
    previous_ = frame;
    drawn_ = true;
}

void Renderer::ColorTest(bool show) {
    color_test_ = show;
    for (auto* color : colors_) {
        if (show)
            lv_obj_remove_flag(color, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(color, LV_OBJ_FLAG_HIDDEN);
    }
}
void Renderer::Destroy() {
    for (auto*& color : colors_) {
        if (color)
            lv_obj_delete(color);
        color = nullptr;
    }
    if (root_)
        lv_obj_delete(root_);
    root_ = nullptr;
}
}  // namespace pocket::face
