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

## SmartElex SD/MODE ambiguity and silence measurements

The owner's exact module is the [Robocraze SmartElex MAX98357A listing](https://robocraze.com/products/smartelex-max98357a-i2s-audio-breakout-amplifier-for-raspberry-pi-and-microcontrollers),
SKU TIFCC0264. Its specifications say 5 V supply. That listing provides no
SD/MODE schematic or open-pin bias specification.

The [SmartElex guide](https://robu-prod-media.s3.ap-south-1.amazonaws.com/uploads/2025/01/SmartElex-I2S-Audio-Breakout-%E2%80%93-MAX98357A.pdf)
lists SD low as enabled and shows SD connected to ground. This conflicts with
the MAX98357A chip datasheet, where SD_MODE low shuts down the amplifier. The
guide also gives gain connections that disagree with the chip's table. It does
not include a schematic establishing an inverted board-level control. Therefore
do not infer the actual module's bias or polarity from that example. Check
whether its labeled SD pin connects directly to chip SD_MODE or through other
circuitry. Leaving SD open requires a verified pull-up; it does not prove enable.

With USB disconnected, check continuity and the breakout's pull-up components.
With power applied, use a multimeter between **amplifier GND and VIN**, then
**amplifier GND and SD/MODE**. Record both at idle and during `tone`. VIN should
be near the intended 5 V and stay within 2.5–5.5 V at the chip. Never power the
INMP441 from this rail; its VDD stays at 3V3. For a confirmed direct chip SD_MODE
connection, 0 V means shutdown; a measured 3.3 V selects left and is comfortably
above the maximum 1.5 V left-channel threshold. Only test that bias after checking
the module circuit and ensuring VIN remains above the control voltage. Disconnect
USB before changing wiring. Do not connect a possibly VIN-pulled-up SD pin to an
ESP32 GPIO. Both transmitted slots contain the same mono signal.

| Point at the amplifier header | Instrument / expected reading |
|---|---|
| VIN–GND | DMM: approximately 5 V; scope: look for dips during tone |
| SD/MODE–GND | DMM: actual bias; compare against verified module circuit and chip thresholds |
| BCLK–GND (GPIO26) | Scope/logic analyzer: 1.024 MHz, ~3.3 V logic, 64 clocks per stereo frame |
| LRCLK–GND (GPIO25) | Scope/logic analyzer: 16 kHz, ~3.3 V logic, 50% duty cycle |
| DIN–GND (GPIO22) | I2S decoder: MSB first, 32-bit stereo, one BCLK delay after WS, identical signed words in both slots during tone |
| SPK+–SPK− | Differential probe, or two correctly grounded probes with scope subtraction: filtered 440 Hz component during tone |

A DMM cannot establish I2S frequency, data framing, or PCM correctness. A
frequency-capable DMM can help with clocks only if rated for 1.024 MHz. Probe
at the amplifier as well as the ESP32 to locate a disconnected jumper. During
the repaired `tone`, signed PCM16 peak should be approximately 120 counts after
gain (stored in the top 16 bits of each 32-bit slot). It runs for 300 ms, with
ramps; use a single-shot scope trigger or issue separate `tone` commands.

Class-D outputs contain a high-frequency switching carrier; measure the
differential **audio component**, not carrier RMS. Never ground SPK+ or SPK−,
including with a scope ground clip. If power, enable and decoded DIN pass but
there is no differential 440 Hz component, inspect the amplifier, output wiring,
and speaker. With power off, disconnect one speaker lead and check its DC
resistance (an 8-ohm speaker is usually somewhat below 8 ohms). An open speaker
coil or wrong terminals cannot be fixed in firmware. Measure below the speaker's
0.5 W rating; the digital cap is not an electrical power guarantee.

Record five separate results: driver initialization, accepted digital TX data,
physical I2S signals, measured amplifier enable/output, and audible playback.
See [repair procedure and acceptance report](AUDIO_REPAIR.md).

| Failed observation | Corrective check / action |
|---|---|
| VIN absent or collapses | Power off; inspect the VIN solder joint, USB power path and common ground. Repair the connection or supply before audio testing. |
| Ground connection open | With power disconnected, check continuity between ESP32, amplifier and microphone grounds; repair loose joints. |
| Direct chip SD_MODE near 0 V | Shutdown; verify the breakout circuit and provide its specified enable bias. Open SD is insufficient if there is no pull-up. |
| Clocks present at ESP32 but absent at a module | Power off; repair the GPIO26/25 wire or module solder joint. |
| Clocks absent at ESP32 | Capture driver startup errors and verify the installed board/firmware; expected signals printed by `diag` are not measurements. |
| Nonzero TX diagnostics but DIN absent at amplifier | Power off; check GPIO22-to-DIN continuity, wrong terminal or a short. Do not reassign GPIOs. |
| Supply, chip enable and decoded DIN valid, no differential audio | Check amplifier condition/thermal protection and output connections; this points beyond successful software transfer. |
| Speaker coil open | Disconnect power and one speaker lead, verify resistance; repair its wire or replace a failed speaker. |
| Mic WAV silent/noisy while RMS is nonzero | Check mic VDD at 3V3, common GND, L/R-to-GND, GPIO34 data continuity, clocks at the mic and the unblocked acoustic port. A floating data wire can give varying noise. |
| Mic WAV repeatedly clips | Check electrical noise first; then lower the configured input gain and compare recordings. Do not treat saturated noise as speech. |

Without measurement equipment, inspect solder joints and trace each wire with
power disconnected, then obtain electrical checks from someone with a meter.
Visual inspection and listening can narrow the problem but cannot verify VIN,
ground continuity, SD/MODE voltage or I2S timing.
