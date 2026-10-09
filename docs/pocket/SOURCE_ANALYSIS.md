# Source and feasibility analysis

Foundation: official `78/xiaozhi-esp32`, upstream commit
`0d576d3d4c049c6f55eaf879725dc23e516511b4`, project version 2.5.1.
SDK: official ESP-IDF v6.1, commit
`fff9895c82d744c7237be8847347bdd1b07c6643`, with its recursive submodules.
The checked-out upstream README, AGENTS.md, and component manifest require
IDF >=6.0.1 and reject 5.x. The resolved component lock is copied into each
release variant for reproducibility.

These source files were inspected before implementation:

| Area | Upstream source retained / assessed |
|---|---|
| Board registration | `docs/custom-board.md`, `main/boards/common/board.*`, `main/Kconfig.projbuild`, `main/CMakeLists.txt`, `scripts/build.py` |
| Closest classic ESP32 board | `main/boards/bread-compact-esp32-lcd/`, its raw-I2S codec and ST7789 configuration |
| Audio interface | `main/audio/audio_codec.*`, `main/audio/codecs/no_audio_codec.*`, `main/audio/README.md` |
| Real Opus and resampling | `main/audio/audio_service.*`, `main/audio/engines/lite_audio_engine.*` |
| Official activation/discovery | `main/ota.*`, `main/application.*`, board/system JSON and NVS settings |
| Conversation lifecycle | `main/device_state_machine.*`, application events, PTT, abort and TTS drain behavior |
| Transports | `main/protocols/websocket_protocol.*`, `mqtt_protocol.*`, `protocol.*`; `docs/websocket.md`, `docs/mqtt-udp.md` |
| Provisioning | `main/boards/common/wifi_board.*`, resolved `78/esp-wifi-connect` 3.3.1 |
| UI | `main/display/lcd_display.*`, LVGL display/theme/font/asset helpers |
| Flash/OTA | `sdkconfig.defaults`, `sdkconfig.defaults.esp32`, `partitions/v2/4m.csv`, OTA write path |

The original upstream board definitions retain their pins and identities.
The new directory registers one factory and is selected through config.json,
the build script, Kconfig, and the CMake board-selection branch. New feature
options retain upstream defaults for other boards.

Resource decision: the stock ESP32 defaults have a 0x2f0000-byte factory app
and a 1 MB assets partition. That layout already has no second OTA app slot.
This custom release removes external assets entirely and expands the factory
slot to 0x3f0000 (4,128,768 bytes), leaving NVS/PHY before 0x10000. Successful
builds and inspected image sizes establish flash feasibility for a 4 MB chip.
Runtime audio/network/UI RAM feasibility still needs physical test M; no heap
peak, speaker quality, or sustained-service result is inferred from linking.

Removed optional resource consumers: PSRAM, wake word and speech models,
device/server AEC, BLE/BluFi, camera, large emoji/GIF assets, image cache,
dynamic server glyphs, and battery logic. The display retains upstream SPI/LVGL
initialization with a single 9,600-byte strip and a fixed set of UI objects.
Subtitles keep 512 bytes and no local conversation history. Font 16_4 is present
in the resolved fonts component; the nonexistent 16_1 profile is not used.
The header uses LVGL's compact Montserrat 20 font.

Audio allocation budget: TX/RX each use six DMA buffers of 128 stereo32 frames,
for 12,288 bytes of sample payload total, plus driver descriptor/queue overhead.
The custom codec owns two 1,024-byte scratch arrays. Opus retains the upstream
16 kHz mono/60 ms uplink, complexity 0, and real encoder/decoder APIs. The
upstream Lite engine feeds raw PCM, and output resampling supports negotiated
rates. The custom shared clock never changes to the cloud sample rate.

Queue limits: two PCM encode tasks, two PCM playback tasks, 40 uplink packets,
20 normal downlink packets (1.2 seconds), and at most 33 local test-history
packets (under two seconds). Packet payloads remain variable-size allocations;
the WebSocket input caps each at 4,096 bytes. This is bounded allocation rather
than a claim of zero runtime allocations. The codec task's conservative upstream
24 KB stack remains; live stack minimum readings determine any future reduction.

Official flow is unchanged: boot -> Wi-Fi hotspot provisioning -> discovery
POST to the upstream default `https://api.tenclass.net/xiaozhi/ota/` -> actual
activation code/challenge -> console registration -> discovered transport.
The custom preference selects WebSocket only when official discovery supplies
its URL/token. MQTT/UDP remains the fallback if that is what the service returns.
No unverified WSS address, fake MAC, fabricated activation code, or replacement
backend is introduced. TLS uses the upstream certificate bundle.

WebSocket keeps the upstream Device-Id, Client-Id, Authorization and
Protocol-Version headers, hello negotiation, session, JSON events and Opus
framing. Version 1 carries raw Opus; versions 2 and 3 use the existing 16-byte
and 4-byte network-order headers. A small host-tested parser checks header,
payload length, type, and bounds without modifying the receive buffer. Hello
rate/duration validation and stale hello-bit cleanup were added. Shared MQTT/UDP
framing and encryption were not replaced.

OTA discovery remains necessary for activation and service configuration, but
both automatic and manual firmware writes return disabled for this board.
There is no inactive app slot, automatic stock-board update, signed OTA, or
rollback promise. USB releases contain ESP image checksums and SHA-256 hashes,
and the bootloader's always-skip-validation option is disabled.

Windows build compatibility: resolved `esp_audio_codec` 2.5.0 uses a raw `-L`
link argument that splits paths containing spaces. Project-owned
`cmake/pocket_codec_paths.cmake` converts that target argument into a proper
CMake link-directory property. No managed/vendor source or generated files were
manually patched. Builds continue to use the official `scripts/build.py` entry.

The reference firmware was re-inspected locally: SHA-256
`3d9dc6d68cc0eebaacb05f20fb4cc49fda7802eda21400483f1d9fcf2a01cf98`.
It has valid ESP32-C3 boot/app images, project `xiaozhi`, version 2.4.0,
ESP-IDF v5.5.2-dirty. It is incompatible with classic ESP32 and is excluded from
the new release. Its binary cannot provide its original source or exact board
pins. The [video](https://youtu.be/25RGnr407PM) page was accessible but provided
no usable transcript; no unobserved wiring or visual behavior is attributed to it.
The new build uses current official source and your supplied wiring.

Primary references: [pinned upstream](https://github.com/78/xiaozhi-esp32/tree/0d576d3d4c049c6f55eaf879725dc23e516511b4),
[custom board guide](https://github.com/78/xiaozhi-esp32/blob/0d576d3d4c049c6f55eaf879725dc23e516511b4/docs/custom-board.md),
[ESP-IDF v6.1](https://github.com/espressif/esp-idf/releases/tag/v6.1),
[XiaoZhi console](https://xiaozhi.me/),
[ESP Web Tools](https://esphome.github.io/esp-web-tools/).
