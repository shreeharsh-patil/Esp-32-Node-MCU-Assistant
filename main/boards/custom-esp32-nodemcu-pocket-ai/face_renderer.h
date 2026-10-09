#pragma once
#include <lvgl.h>
#include <array>
#include "face_animation.h"

namespace pocket::face {
// Shared by firmware and the native LVGL screenshot harness.
class Renderer {
public:
    explicit Renderer(lv_obj_t* screen);
    ~Renderer() { Destroy(); }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void Draw(const Frame& frame);
    void ColorTest(bool show);
    void Destroy();

private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* plate_ = nullptr;
    std::array<lv_obj_t*, 2> rim_{};
    struct Eye {
        std::array<lv_obj_t*, 2> glow{};
        lv_obj_t* body = nullptr;
        lv_obj_t* highlight = nullptr;
        lv_obj_t* sparkle = nullptr;
        lv_obj_t* smile = nullptr;
        std::array<lv_point_precise_t, 9> points{};
    };
    std::array<Eye, 2> eyes_{};
    std::array<std::array<lv_obj_t*, 5>, 2> cheeks_{};
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* smile_ = nullptr;
    std::array<lv_point_precise_t, 9> smile_points_{};
    std::array<lv_obj_t*, 3> dots_{}, colors_{};
    Frame previous_{};
    bool drawn_ = false, color_test_ = false;
};
}  // namespace pocket::face
