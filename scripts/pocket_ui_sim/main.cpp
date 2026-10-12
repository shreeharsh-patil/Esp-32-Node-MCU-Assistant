#include <lvgl.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "clock_renderer.h"
#include "face_animation.h"
#include "face_renderer.h"
#include "panel_diagnostics.h"

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
void CheckPartialMatchesFull(lv_display_t* display, lv_obj_t* screen,
                             const std::filesystem::path& output, const char* state,
                             uint64_t time) {
    const auto partial = pixels;
    lv_obj_invalidate(screen);
    lv_refr_now(display);
    // LVGL's translucent rounded polyline caps can rasterize differently at
    // two isolated edge pixels when the strip clipping rectangle changes.
    // Bound the *total* differing pixels, with no image-wide color tolerance:
    // a retained line or rectangular patch must fail this comparison.
    unsigned differences = 0;
    for (unsigned i = 0; i < pixels.size(); ++i)
        differences += pixels[i] != partial[i];
    const bool matches = differences <= 4;
    if (!matches) {
        int x1 = kWidth, y1 = kHeight, x2 = 0, y2 = 0, count = 0;
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                if (pixels[y * kWidth + x] != partial[y * kWidth + x]) {
                    ++count;
                    x1 = std::min(x1, x);
                    y1 = std::min(y1, y);
                    x2 = std::max(x2, x);
                    y2 = std::max(y2, y);
                }
        std::fprintf(stderr, "Stale partial redraw: %s time=%llu pixels=%d bounds=%d,%d..%d,%d\n",
                     state, static_cast<unsigned long long>(time), count, x1, y1, x2, y2);
        Save(output / "panel-stale-full.ppm");
        const auto full = pixels;
        pixels = partial;
        Save(output / "panel-stale-partial.ppm");
        pixels = full;
    }
    assert(matches);
}
void CheckClock(lv_display_t* display, lv_obj_t* screen, const std::filesystem::path& output) {
    pocket::ClockRenderer clock(screen);
    Renderer face(screen);
    Engine engine(7);
    face.Draw(engine.Step(2000, Mode::Idle, 0, 0, false));
    face.Visible(false);
    clock.Visible(true);
    std::tm local{};
    local.tm_year = 126;
    local.tm_mon = 9;
    local.tm_mday = 12;
    local.tm_wday = 1;
    local.tm_hour = 22;
    local.tm_min = 28;
    auto text = pocket::FormatClock(1791844080, local);
    clock.Update(text);
    lv_tick_inc(50);
    lv_refr_now(display);
    Save(output / "clock-reference.ppm");
    auto before = flushes;
    for (int i = 0; i < 100; ++i) {
        clock.Update(text);
        lv_tick_inc(50);
        lv_refr_now(display);
    }
    assert(flushes == before);  // No repeated full-screen or per-second redraw.
    auto before_pixels = transferred_pixels;
    local.tm_min = 29;
    clock.Update(pocket::FormatClock(1791844140, local));
    lv_tick_inc(50);
    lv_refr_now(display);
    assert(transferred_pixels > before_pixels);
    assert(transferred_pixels - before_pixels < kWidth * kHeight);
    CheckPartialMatchesFull(display, screen, output, "clock minute", 0);
    Save(output / "clock-minute.ppm");
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_mday = 13;
    local.tm_wday = 2;
    clock.Update(pocket::FormatClock(1791844200, local));
    lv_tick_inc(50);
    lv_refr_now(display);
    Save(output / "clock-midnight.ppm");
    clock.Update(pocket::ClockText{});
    lv_tick_inc(50);
    lv_refr_now(display);
    Save(output / "clock-sync.ppm");
    // All 12h layouts and repeated transitions through real LVGL. No cropped
    // flush rectangle, stale digits, accumulating objects, or leaked font RAM.
    lv_mem_monitor_t initial{};
    lv_mem_monitor(&initial);
    for (int i = 0; i < 100; ++i) {
        local.tm_hour = i % 24;
        local.tm_min = i % 60;
        face.Visible(false);
        clock.Visible(true);
        clock.Update(pocket::FormatClock(1791844200, local));
        lv_tick_inc(50);
        lv_refr_now(display);
        clock.Visible(false);
        face.Visible(true);
        lv_tick_inc(50);
        lv_refr_now(display);
        assert(pixels[0] == 0 && pixels[kWidth * kHeight - 1] == 0);
    }
    Save(output / "clock-return-face.ppm");
    lv_mem_monitor_t final{};
    lv_mem_monitor(&final);
    assert(final.free_size >= initial.free_size);
    std::printf(
        "CLOCK LVGL PASS: unchanged time 0 flushes; minute partial redraw; 100 round trips; "
        "free=%zu max_used=%zu\n",
        final.free_size, final.max_used);
}
void CheckPanelDiagnostics(lv_display_t* display, lv_obj_t* screen,
                           const std::filesystem::path& output) {
    lv_mem_monitor_t before{};
    lv_mem_monitor(&before);
    {
        pocket::PanelDiagnostics diagnostics(screen);
        for (unsigned stage = 0; stage < pocket::kPanelTestStages; ++stage) {
            const auto pattern = static_cast<pocket::PanelPattern>(stage);
            // Old pixels must be overwritten, including every edge and corner.
            pixels.fill(0x5a5a);
            diagnostics.Show(pattern);
            lv_tick_inc(50);
            lv_refr_now(display);
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    uint16_t expected = 0;
                    switch (pattern) {
                        case pocket::PanelPattern::White:
                        case pocket::PanelPattern::ClearWhite:
                            expected = 0xffff;
                            break;
                        case pocket::PanelPattern::Red:
                            expected = 0xf800;
                            break;
                        case pocket::PanelPattern::Green:
                            expected = 0x07e0;
                            break;
                        case pocket::PanelPattern::Blue:
                            expected = 0x001f;
                            break;
                        case pocket::PanelPattern::Checkerboard:
                            expected = (x / 16 + y / 16) % 2 == 0 ? 0xffff : 0;
                            break;
                        case pocket::PanelPattern::Horizontal:
                            expected = y % 12 == 0 ? 0xffff : 0;
                            break;
                        case pocket::PanelPattern::Vertical:
                            expected = x % 12 == 0 ? 0xffff : 0;
                            break;
                        default:
                            break;
                    }
                    assert(pixels[y * kWidth + x] == expected);
                }
            }
            Save(output / ("panel-" + std::to_string(stage + 1) + ".ppm"));
            const auto flushed = flushes;
            for (unsigned tick = 0; tick < 60; ++tick) {
                diagnostics.Show(pattern);
                lv_tick_inc(50);
                lv_refr_now(display);
            }
            assert(flushes == flushed);  // Stable test holds do not refresh.
        }
        diagnostics.Hide();
        lv_tick_inc(50);
        lv_refr_now(display);
        for (const auto pixel : pixels)
            assert(pixel == 0);  // Opaque normal screen restored, no test bars.
    }
    lv_tick_inc(50);
    lv_refr_now(display);
    lv_mem_monitor_t after{};
    lv_mem_monitor(&after);
    assert(after.free_size >= before.free_size);
    std::puts(
        "PANEL LVGL PASS: all 67200 pixels of 10 patterns; no hold redraw; full clear; no leak");
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
                    CheckPartialMatchesFull(display, screen, output, state.name, now);
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
    CheckClock(display, screen, output);
    CheckPanelDiagnostics(display, screen, output);
    lv_display_delete(display);
    lv_deinit();
}
