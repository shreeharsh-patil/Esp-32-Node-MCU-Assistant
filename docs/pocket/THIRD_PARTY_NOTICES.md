# License and dependency attribution

The upstream XiaoZhi repository and its original MIT LICENSE are retained.
Custom board code, build compatibility shim, release tools and local changes
are distributed under that same MIT license.

Firmware includes ESP-IDF 6.1, its applicable component licenses, and the managed
components selected by the official component manager. Each generated release
copies available LICENSE/COPYING/NOTICE files from managed components and the
SDK into `pocket-release/licenses/`, alongside a dependency lock and inventory.
These notices include the actual upstream license texts rather than replacing
them with this summary.

Major direct runtime dependencies include LVGL 9.5.0, esp_lvgl_port 2.9.0,
esp_audio_codec 2.5.0, esp_audio_effects 1.3.x, esp-wifi-connect 3.3.1,
esp-ml307's ESP32 network helpers, and xiaozhi-fonts 2.0.0. The generated lock
provides exact versions and hashes. Library packages may contain modules for
other devices; the classic ESP32 build and linker determine which are used.
No external codec chip is initialized by this board.

Some Espressif prebuilt audio libraries carry an Espressif-specific MIT license
restricting use to Espressif products. This release targets an Espressif ESP32;
preserve those exact notices if redistributing. No cloud service license or
terms are substituted for the source licenses.

The optional installer loads the official ESP Web Tools browser module at its
documented version-10 CDN URL. It is not embedded into the source archive.
Upstream assets and font notices remain with their components/source files.
The incompatible third-party reference firmware is not redistributed.
