#include "pocket_panel.h"
#include <esp_heap_caps.h>
#include <esp_lcd_panel_commands.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include "config.h"

esp_err_t InitializePocketPanel(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel) {
    auto error = esp_lcd_panel_reset(panel);
    // IDF's hardware reset releases RESX for 10 ms. ST7789V requires
    // 120 ms before Sleep Out, including a warm ESP32-only restart.
    if (error == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(120));
        error = esp_lcd_panel_init(panel);
    }
    if (error == ESP_OK) {
        // The SDK already waits 100 ms after Sleep Out; complete 120 ms.
        vTaskDelay(pdMS_TO_TICKS(20));
        error = esp_lcd_panel_disp_on_off(panel, false);
    }
    if (error == ESP_OK)
        error = esp_lcd_panel_invert_color(panel, DISPLAY_INVERT);
    if (error == ESP_OK)
        error = esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    if (error == ESP_OK)
        error = esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    if (error != ESP_OK)
        return error;

    // Reset does not erase GRAM. Clear the entire 320x240 rotated controller
    // RAM, including the 20-column insets outside the 280x240 visible window.
    // A constant 640-byte DMA row avoids another framebuffer/large allocation.
    constexpr int ram_width = 320, ram_height = 240;
    auto* row = static_cast<uint16_t*>(
        heap_caps_calloc(ram_width, sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!row)
        return ESP_ERR_NO_MEM;
    for (int y = 0; y < ram_height && error == ESP_OK; ++y)
        error = esp_lcd_panel_draw_bitmap(panel, 0, y, ram_width, y + 1, row);
    // Parameter transactions drain pending color DMA. Never free/reuse row
    // while the final asynchronous SPI transfer still references it.
    const auto drained = esp_lcd_panel_io_tx_param(io, LCD_CMD_NOP, nullptr, 0);
    heap_caps_free(row);
    if (error == ESP_OK)
        error = drained;
    ESP_LOGI("PocketPanel",
             "ST7789 init %s: SPI=%dHz RGB565 RGB invert=%d; reset settle=120ms; "
             "GRAM=320x240 black; visible=%dx%d offset=%d/%d",
             esp_err_to_name(error), DISPLAY_SPI_HZ, DISPLAY_INVERT, DISPLAY_WIDTH, DISPLAY_HEIGHT,
             DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
    return error;
}
