# Hardware Notes

Validated on Tian's physical ESP32-S3 board on 2026-07-05.

## Onboard RGB LED

- GPIO: 48
- Device: onboard addressable RGB LED / WS2812-style LED
- Evidence: `rgb_led_set` with `MIMI_RGB_LED_GPIO=48` changes the visible onboard LED.
- Do not change this board to GPIO38 based only on generic ESP32-S3-DevKitC-1 docs or pinout images. Some board revisions use GPIO38, but this physical board uses GPIO48.

## Buttons

- BOOT: GPIO0
- Released level: HIGH
- Pressed level: LOW
- Evidence: the BOOT-to-RGB test task detected presses and changed the GPIO48 RGB color.
- RST/EN is reset only; it is not a readable GPIO input.

## External I2S Audio

- Device: NS4168 I2S DAC / mono Class-D amplifier and speaker.
- Wiring: V=3V3, G=GND, BCL/BCLK=GPIO16, LRC/LRCLK=GPIO17, DIN/SDATA=GPIO18. On this physical board the header marked 5V behaves as input-only; 3V3 is the validated supply for the amplifier.
- Purpose: exclusively transmits successfully sent Telegram replies as `ggwave` data. It is not exposed as a tool and has no connection to RGB effects or semantic light signals.
- Behavior: transmits the complete final Telegram reply using ggwave's audible-fastest multi-tone waveform. Long UTF-8 replies are split into phrases of at most 64 bytes, preferring sentence punctuation, then commas and word boundaries, without splitting a codepoint. Short adjacent sentences are combined until roughly 30 bytes; sentence pauses are 140 ms and clause pauses are 70 ms. I2S remains enabled and emits digital silence between phrases to avoid amplifier pops from repeatedly stopping its clocks. The temporary `Heinz is working...` status never triggers audio.
