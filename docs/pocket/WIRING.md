# Wiring and electrical checks

Keep the existing GPIO plan. All signals are 3.3 V logic and all grounds are common.

| Device terminal | ESP32 terminal |
|---|---|
| TFT VCC, BLK | 3V3 |
| TFT GND | GND |
| TFT SCL (SPI SCK) | GPIO18 |
| TFT SDA (SPI MOSI) | GPIO23 |
| TFT CS | GPIO13 |
| TFT DC | GPIO27 |
| TFT RES | GPIO14 |
| INMP441 VDD | 3V3 |
| INMP441 GND, L/R | GND |
| INMP441 SCK | GPIO26 |
| INMP441 WS | GPIO25 |
| INMP441 SD (data output) | GPIO34 |
| MAX98357A VIN | Verified USB-derived 5 V/VIN |
| MAX98357A GND | GND |
| MAX98357A BCLK | GPIO26, shared with microphone |
| MAX98357A LRC | GPIO25, shared with microphone |
| MAX98357A DIN | GPIO22 |
| Speaker positive | MAX98357A SPK+ |
| Speaker negative | MAX98357A SPK- |

GPIO34 is input-only and is used exclusively as mic data input. No imaginary
backlight GPIO, I2C codec, touch input, battery ADC, or charging pin is initialized.

Measure the exact NodeMCU VIN pin with USB connected before connecting amplifier
VIN. Confirm approximately 5 V rather than assuming every board routes USB to
VIN the same way. Do not substitute the 3V3 rail for the specified amplifier supply.

Leave GAIN and SD unconnected initially only after checking the actual amplifier
breakout schematic or resistance/bias. A bare MAX98357A has SD/MODE behavior that
depends on its bias; some breakouts provide the needed pull-up and others do not.
An unconnected bare shutdown input is not a universal normal-playback setting.
Do not pull SD directly to ground for normal playback. Identify the breakout
before selecting any bias resistor. Floating GAIN normally selects 9 dB on the
bare chip; do not add a maximum-gain connection.

For a confirmed MAX98357A powered from USB-derived 5 V, an explicit enable test
can connect the amplifier's SD/SD_MODE input to ESP32 3V3. Disconnect USB before
changing the wire, then reconnect. A high SD_MODE selects the left channel;
this firmware supplies identical mono audio in both slots. This is the
amplifier enable input, not the INMP441 SD data output on GPIO34. Verify the
breakout's labeled pin before making the connection. The current device's user
reported an unconnected amplifier SD pin; audible results after enabling it
are recorded separately in DEVICE_DIAGNOSTICS_RESULTS.json.

The MAX98357A drives a differential bridge output: neither SPK- nor SPK+ is
circuit ground. Do not attach either terminal to an ESP32 GPIO. Use a suitable
differential measurement method for speaker voltage. A ground-referenced scope
probe across a bridge output can short an amplifier terminal to ground.

The 8-ohm 0.5 W speaker corresponds to about 2.0 Vrms for a continuous sine
wave (sqrt(0.5 * 8)). This calculation is not an output measurement. Start with
the firmware's low default volume, check audible distortion and differential
output, and stay comfortably below the speaker's rating before increasing volume.
The absolute digital peak cap does not replace power measurement because breakout
gain, supply, waveform, and load affect actual power.

Shared-clock implementation: paired TX/RX on I2S0, 16 kHz WS, 32-bit left and
right slots, 1.024 MHz BCLK. The INMP441's signed 24-bit left samples are MSB
aligned. The speaker receives the same attenuated mono word in both slots, so
either valid breakout channel-selection bias can reproduce it. No second master
drives GPIO26/25. Upstream resampling converts server audio to the fixed hardware
rate; a 24 kHz server response does not change the wire clock to 24 kHz.

Relevant primary references:
[ESP32 I2S full duplex](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/peripherals/i2s.html),
[MAX98357A datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/max98357a-max98357b.pdf),
[ESP32 boot mode/GPIO0](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html).

Future battery architecture remains optional: protected 1-cell LiPo, suitable
charger, regulated board/amplifier supply, power switch, and protected voltage
measurement. A raw LiPo is not a substitute for this NodeMCU's USB/VIN input,
and this firmware does not charge or measure a battery.
