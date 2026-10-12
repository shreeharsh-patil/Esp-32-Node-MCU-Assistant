#pragma once
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

// Board-owned timing/GRAM initialization; does not alter the SDK driver.
esp_err_t InitializePocketPanel(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);
