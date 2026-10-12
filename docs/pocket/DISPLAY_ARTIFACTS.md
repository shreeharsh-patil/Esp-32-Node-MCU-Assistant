# ST7789 display artifact investigation

Date: 2026-10-12. Branch: `repair/st7789-display-artifacts`.
Target: the existing classic ESP32 NodeMCU/WROOM-32, 4 MB flash, no PSRAM.

## Findings and limits

The owner reports faint vertical lines, a long line, a rectangular shadow and
possible retention during startup and on the clock. Earlier the owner confirmed
the clock and blue/purple background were visible. That confirms output, not
uniformity or panel recovery. No photograph of the newly reported artifacts,
voltage measurement, reset waveform or panel readback has yet been obtained.

Two firmware configuration problems were found and corrected:

1. **The Pocket SPI clock was 26 MHz.** The ST7789V reference specifies a minimum
   66 ns four-wire write cycle, about 15.15 MHz maximum under its stated loading
   conditions. 26 MHz is outside that reference specification. The repair uses
   **10 MHz**, also improving margin for the long, hand-soldered wiring visible
   in the owner's earlier pictures. A different ST7789 variant may have different
   limits; the physical module model has not been confirmed. This is an identified
   configuration risk, not proof that it caused the observed shadow.
2. **Reset recovery was too short before Sleep Out.** The installed ESP-IDF 6.1
   ST7789 driver holds hardware reset low for 10 ms, releases it for 10 ms, then
   sends Sleep Out from `panel_init`. The ST7789V reference requires waiting
   120 ms before Sleep Out following reset. The board now adds 120 ms after
   `panel_reset`, including warm ESP32-only restarts. The driver already waits
   100 ms after Sleep Out; the board adds another 20 ms before panel setup/clear.

Reset does not clear frame memory. The inherited display constructor already
cleared the visible window correctly, but only that window and without checking
each clear return code. The board now first blanks and explicitly clears the
**entire rotated 320×240 controller RAM**, including hidden inset columns, checks
every write and drains queued SPI DMA before freeing its 640-byte constant row.
The inherited visible-window clear and display-on still run afterward.
This makes initial RAM contents deterministic; retention caused by the glass,
power or panel driving is not repaired by clearing RAM.

## Firmware audit

The checkout was verified against the named GitHub repository. Inspection covered
the selected board's complete initialization, pin configuration and variants,
application/display lifecycle, common SPI/LVGL display setup, installed SDK
ST7789 and SPI-I/O drivers, LVGL port flush completion, face animation/renderer,
clock renderer/state, caption bounds and memory ownership. Other board drivers
are not active in this build and are not reconfigured.

| Item | Verified configuration / result |
|---|---|
| Controller | Firmware installs the ESP-IDF ST7789 family driver; exact physical chip/module revision remains unverified |
| Panel/library | 240×280 visible glass, displayed as 280×240 landscape; LVGL 9 through `esp_lvgl_port` and `esp_lcd` |
| SPI | SPI3 host, mode 0; SCK GPIO18, MOSI GPIO23, CS GPIO13, DC GPIO27; no MISO/readback connection |
| Reset | Active-low GPIO14, SDK 10 ms pulse; added 120 ms recovery before init |
| Geometry | Axis exchange enabled; offset X=20/Y=0; default mirror X=true/Y=false; visible columns 20…299, rows 0…239 within 320×240 rotated RAM |
| Color | RGB order, RGB565/COLMOD=0x55; inversion on; LVGL swaps RGB565 bytes once for wire order; black startup data is endian-independent |
| Backlight | BLK tied to 3V3; no software-controlled backlight pin |
| Pixel buffer | One internal DMA 280×20 RGB565 strip, 11,200 bytes; stride alignment 1; no full framebuffer/PSRAM |
| DMA ownership | LVGL marks a SPI flush ready from actual color-transfer completion, not immediately on queueing; new startup row remains alive until the final transfer is drained |
| Scene ownership | One opaque screen; fixed face/clock roots; captions and other overlays hidden appropriately; clock's draw callback restores its temporary clipping area |
| Refresh | Unchanged clock causes zero flushes; minute change redraws only affected content; diagnostics holds also cause zero repeated flushes |

The geometry, offsets, color order and inversion already match the observed
orientation/colors and were retained. Changing these blindly can create new
artifacts. No GPIO assignments, audio clocks, Wi-Fi settings, clock colors,
fonts, face expressions or assistant behavior were changed for this repair.

There is no tearing-effect (TE) wire. A moving horizontal seam during animations
can be asynchronous panel scanning rather than retained pixels. Lower SPI speed
does not guarantee tear-free movement; it primarily addresses signal/timing
margin. A stationary shadow on held solids needs a different investigation.

## Diagnostic mode

`displaytest` starts a nonblocking, automatic 30-second sequence. Each stage lasts
approximately three seconds. Only the diagnostic overlay is drawn: the face,
clock and captions are hidden, and their animations do not redraw behind it.
Wi-Fi and audio tasks keep running. At completion the previous face/clock is
restored. Repeating the sequence, changing to a held solid or cancelling does
not allocate another screen/framebuffer. The offline diagnostic build starts
the sequence automatically once, after its boot animation; normal firmware
starts it only on request.

| Stage | Display | Observation to record |
|---|---|---|
| 1 | Black | Faint lines, glow/rectangular patches, old image |
| 2 | White | Dark rows/columns, dots, shadow |
| 3–5 | Red, green, blue | Correct order, uniform fill, missing color components |
| 6 | 16-pixel checkerboard | Even cells, clean borders and correct clipping |
| 7 | One-pixel horizontal lines every 12 pixels | Unwanted extra/shifted rows |
| 8 | One-pixel vertical lines every 12 pixels | Unwanted extra/shifted columns |
| 9 | Full white overwrite | Clearing preparation |
| 10 | Full black overwrite | No checkerboard/lines/white rectangle left behind |

Serial prints the stage name, number and hold time. **A stage transmitted is not
a visual PASS.** It always labels the physical result pending until observed.
Use `display black` / `display white` for longer inspection, `home` to restore
the face, or `clock` to restore the clock. Test output contains no credentials.

## Manual hardware checks and interpretation

1. Disconnect USB before inspecting wiring. Keep VCC and BLK on **3V3**, GND on
   common GND, and the GPIO connections in the table. Do not try 5 V at display
   power or logic without the actual module's regulator/level-shifter specification.
   The bare reference ST7789V uses 1.65–3.3 V I/O and 2.4–3.3 V supply; this does
   not establish the input rating of every breakout module.
2. Check the display's VCC/GND, reset, CS, DC, SCK and MOSI joints with power off.
   Use short signal wires and a short ground return alongside the SPI wires;
   keep them away from amplifier/speaker switching wiring. Do not press the
   glass or move live wires as a test. Loose ground/reset can resemble a bad LCD.
3. With a borrowed meter if available, measure display VCC–GND at the module
   during idle and speech. With a scope/logic analyzer, verify a clean 10 MHz
   SCK, ~3.3 V logic, active-low reset pulse/recovery, and stable supply. A serial
   message proves the driver requested reset, not that the electrical reset wire
   reached the module. The owner previously reported having no multimeter.
4. Record whether each artifact stays in the **same physical position** across
   solid colors, full clears, the clock and a cold power cycle. Leave USB unplugged
   for roughly 30 seconds before reconnecting. Confirm both a cold start and EN
   restart. Save the result for each, rather than comparing only animations.
5. If lines disappear with the corrected initialization/10 MHz and clean wiring,
   firmware or signal integrity was a contributor. If corruption varies between
   runs, responds to grounding or occurs during speaker output, investigate power,
   reset and SPI integrity before blaming the panel.
6. If a fixed line/patch survives solid-color fills and full clearing at the same
   location after cold starts with verified power/reset/wiring, the evidence favors
   a panel column/row driver, flex/bond or glass problem. Pressure/heat damage is
   possible only with supporting history/inspection; symptoms alone do not prove it.
   A fading previous image may be temporary retention. A persistent defective
   column or pressure mark cannot be repaired by rewriting pixels or running
   prolonged flashing patterns. Replacement is justified only after these checks.

**Recovery assessment:** firmware recovery remains plausible because real timing
and initialization risks were found. Physical recovery is unconfirmed until the
owner observes the new solids and clearing test. No replacement is currently
required based on the evidence available.

## Build, flash and test

The local installer preserves NVS/Wi-Fi/device settings by writing only
bootloader, partition table and application. Do not use `--erase-all`.
Close other serial monitors first, release BOOT, and use a data-capable USB cable.
These commands run from `D:\Pocket Mini\XiaoZhi-Pocket-ESP32`:

```powershell
$env:IDF_TOOLS_PATH='D:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH='D:\Espressif\tools\python_env\idf6.1_py3.11_env'
$env:PYTHONUTF8='1'
. 'D:\Espressif\esp-idf-v6.1\export.ps1'
python scripts/pocket_flash.py --port COM4
python scripts/pocket_serial_capture.py --port COM4 --reset --seconds 45 --command displaytest --command diag --output "$env:TEMP/pocket-display-test.log"
```

Replace COM4 only if Windows assigns a different port. The flasher verifies
release hashes, classic ESP32 identity and at least 4 MB flash before writing.
At 115200 baud, flashing may take several minutes. Optional `--baud 460800`
was used for this repair's connected-device installation.

For held solids, use the 115200-baud serial console and type `display black`,
then `display white`, then `clock` or `home`. Do not leave two programs opening
the COM port. A three-second sequence is for observation, not an LCD repair cycle.

To rebuild source, select the canonical conversation variant, then snapshot it
**before** building another variant:

```powershell
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai
python scripts/pocket_snapshot.py conversation
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai-diagnostics
python scripts/pocket_snapshot.py diagnostics
python scripts/pocket_package.py
```

Packaging regenerates the checked release hashes. The diagnostic image can be
flashed with `scripts/pocket_flash.py --port COM4 --diagnostics`; it disables
Wi-Fi/cloud for independent hardware tests. Reinstall the conversation image
afterward to restore assistant operation, without erasing settings.

## Validation and changed files

The machine-readable outcomes, image hashes and physical observations are in
[`DISPLAY_VALIDATION.json`](DISPLAY_VALIDATION.json). Builds/visual observations
are reported separately. No merge is authorized before validation succeeds.

| File | Change |
|---|---|
| `config.h` | Conservative 10 MHz Pocket SPI clock |
| `pocket_panel.h/.cc` | Reset recovery, blanking, checked full-GRAM clearing, DMA drain and init log |
| `pocket_board.cc` | Use board-owned init; serial diagnostic commands |
| `panel_diagnostics.h/.cc` | Opaque, bounded full-screen solid/pattern renderer |
| `pocket_display.h/.cc` | Nonblocking sequence, held solids, hide/restore original scene |
| `scripts/pocket_serial_capture.py` | Timed test/held-solid commands and sequence spacing |
| `scripts/pocket_ui_sim/main.cpp`, `CMakeLists.txt` | Exact pattern pixels, clearing/no-redraw/no-leak checks and partial-vs-full rendering comparison |
| Board README, this report, validation files | Settings, flash/test instructions and measured results |

Native diagnostics compare every pixel exactly. The face comparison permits at
most four isolated differing RGB565 pixels per frame for LVGL rounded-end-cap
clipping differences, rather than applying a tolerance to whole images. It checks
all animation frames for retained lines/patches and keeps opaque edge checks.
The clock still passes 100 clock/face transitions with stable memory and partial
minute refresh. Those tests do not measure panel voltages, retention or damage.

Sources: [Sitronix ST7789V specification, serial/reset timing and GRAM](https://dl.espressif.com/dl/schematics/ST7789V_SPEC_V1.0.pdf),
[Espressif SPI LCD configuration](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd/spi_lcd.html).
The installed SDK driver source is `D:\Espressif\esp-idf-v6.1\components\esp_lcd\src\esp_lcd_panel_st7789.c`;
its queued-SPI completion behavior is in `components/esp_lcd/spi/esp_lcd_panel_io_spi.c`.
