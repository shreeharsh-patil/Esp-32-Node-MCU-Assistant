#include <lvgl.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "face_animation.h"
#include "face_renderer.h"

LV_FONT_DECLARE(font_noto_sans_basic_16_4);
using namespace pocket::face;
namespace {
std::array<uint16_t, kWidth * kHeight> pixels{};
alignas(64) std::array<uint8_t, kWidth * 20 * 2> strip{};
uint32_t flushes = 0;
uint64_t transferred_pixels = 0;
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* source) {
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < kWidth && area->y2 < kHeight);
    const int width = lv_area_get_width(area);
    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            uint16_t pixel;
            std::memcpy(&pixel, source + (y - area->y1) * stride + (x - area->x1) * 2, 2);
            pixels[y * kWidth + x] = pixel;
        }
    }
    ++flushes;
    transferred_pixels += lv_area_get_size(area);
    lv_display_flush_ready(display);
}
void Save(const std::filesystem::path& path) {
    auto* file = std::fopen(path.string().c_str(), "wb");
    assert(file);
    std::fprintf(file, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (const auto pixel : pixels) {
        const uint8_t rgb[] = {static_cast<uint8_t>(((pixel >> 11) & 31) * 255 / 31),
                               static_cast<uint8_t>(((pixel >> 5) & 63) * 255 / 63),
                               static_cast<uint8_t>((pixel & 31) * 255 / 31)};
        std::fwrite(rgb, 1, 3, file);
    }
    std::fclose(file);
}
}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path output = argc > 1 ? argv[1] : "ui-preview";
    std::filesystem::create_directories(output);
    lv_init();
    auto* display = lv_display_create(kWidth, kHeight);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, strip.data(), nullptr, strip.size(),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, Flush);
    auto* screen = lv_screen_active();
    pixels.fill(0xffff);  // Model old white GRAM; startup must overwrite it.
    const struct {
        const char* name;
        Mode mode;
        const char* caption;
    } states[] = {{"idle", Mode::Idle, ""},
                  {"listening", Mode::Listening, ""},
                  {"thinking", Mode::Thinking, ""},
                  {"speaking", Mode::Speaking, ""},
                  {"happy", Mode::Happy, ""},
                  {"confused", Mode::Confused, ""},
                  {"sleepy", Mode::Sleepy, ""},
                  {"error", Mode::Error, "Connection lost.\nTrying again..."},
                  {"setup", Mode::Setup, "Join Shreeharsh Assistant\nKey: 0123456789ABCDEF"},
                  {"pairing-layout", Mode::Setup, "Open 192.168.4.1\nChoose 2.4GHz Wi-Fi"},
                  {"curious", Mode::Curious, ""},
                  {"shy", Mode::Shy, ""},
                  {"surprised", Mode::Surprised, ""},
                  {"processing", Mode::Processing, ""}};
    size_t stable_free = 0;
    for (int cycle = 0; cycle < 4; ++cycle) {
        {
            Renderer renderer(screen);
            auto* label = lv_label_create(screen);
            lv_obj_set_pos(label, 10, 185);
            lv_obj_set_size(label, 260, 49);
            lv_obj_set_style_text_font(label, &font_noto_sans_basic_16_4, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(kEye), 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            for (const auto& state : states) {
                Engine engine(7);
                Frame final_frame;
                if (state.caption[0])
                    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text_static(label, state.caption);
                const uint64_t duration = state.mode == Mode::Surprised ? 600 : 2400;
                for (uint64_t now = 0; now < duration; now += 33) {
                    const auto f =
                        engine.Step(now, state.mode, state.mode == Mode::Listening ? 2400 : 0,
                                    state.mode == Mode::Speaking ? 5200 : 0, state.caption[0]);
                    final_frame = f;
                    renderer.Draw(f);
                    lv_tick_inc(33);
                    lv_refr_now(display);
                    // Full opaque black remains at every edge during all
                    // expressions and partial redraws, including boot fades.
                    for (int x = 0; x < kWidth; ++x) {
                        assert(pixels[x] == 0);
                        assert(pixels[(kHeight - 1) * kWidth + x] == 0);
                    }
                    for (int y = 0; y < kHeight; ++y) {
                        assert(pixels[y * kWidth] == 0);
                        assert(pixels[y * kWidth + kWidth - 1] == 0);
                    }
                }
                // RGB565 rounding must preserve pink, including dim sleepy blush.
                for (const auto& cheek : final_frame.cheeks) {
                    const uint16_t pixel =
                        pixels[(cheek.y + cheek.h / 2) * kWidth + cheek.x + cheek.w / 2];
                    const int red = ((pixel >> 11) & 31) * 255 / 31;
                    const int green = ((pixel >> 5) & 63) * 255 / 63;
                    const int blue = (pixel & 31) * 255 / 31;
                    assert(red > green && red > blue && red < 240);
                }
                if (cycle == 0)
                    Save(output / (std::string(state.name) + ".ppm"));
            }
            Engine boot(7);
            for (uint64_t now = 0; now < 1250; now += 33) {
                renderer.Draw(boot.Step(now, Mode::Boot, 0, 0, false));
                lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
                lv_tick_inc(33);
                lv_refr_now(display);
                if (cycle == 0 && now % 99 == 0)
                    Save(output / ("boot-" + std::to_string(now) + ".ppm"));
            }
            // Continuous audio-reactive sequence through the real renderer.
            // Only the input levels are simulated; no fake PCM reaches firmware.
            Engine reactive(23);
            for (uint64_t now = 0; now < 10560; now += 66) {
                const Mode mode = now < 2508   ? Mode::Listening
                                  : now < 4488 ? Mode::Thinking
                                  : now < 8580 ? Mode::Speaking
                                               : Mode::Idle;
                const uint32_t microphone = mode == Mode::Listening ? 3000 : 0;
                const uint32_t playback =
                    mode == Mode::Speaking && now % 990 < 660
                        ? static_cast<uint32_t>(1000 + 7500 * (0.5 + 0.5 * std::sin(now / 100.0)))
                        : 0;
                renderer.Draw(reactive.Step(now, mode, microphone, playback, false));
                lv_tick_inc(66);
                lv_refr_now(display);
                if (cycle == 0)
                    Save(output / ("reactive-" + std::to_string(now / 66) + ".ppm"));
            }
            renderer.ColorTest(true);
            lv_tick_inc(33);
            lv_refr_now(display);
            renderer.ColorTest(false);
            lv_obj_delete(label);
        }
        lv_tick_inc(33);
        lv_refr_now(display);
        lv_mem_monitor_t memory{};
        lv_mem_monitor(&memory);
        if (cycle == 1)
            stable_free = memory.free_size;
        if (cycle > 1)
            assert(memory.free_size >= stable_free);
    }
    std::printf(
        "PASS: shared LVGL renderer, RGB565 11200B strip, bounded flushes=%u pixels=%llu; repeated "
        "create/destroy stable_free=%zu\n",
        flushes, static_cast<unsigned long long>(transferred_pixels), stable_free);
    lv_display_delete(display);
    lv_deinit();
}
