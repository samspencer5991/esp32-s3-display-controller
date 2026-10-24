# Hardware: LilyGO T-Display-S3-Long

| Part | Details |
|---|---|
| MCU | ESP32-S3R8, dual core at 240 MHz, 16 MB flash, 8 MB octal PSRAM |
| Display | 3.4" 180×640 IPS, AXS15231B controller over QSPI (portrait native; the UI runs rotated to 640×180 landscape) |
| Touch | Original boards: AXS15231B built-in controller (I2C `0x3B`). 2026+ revisions: Hynitron CST3530 (I2C `0x58`). The firmware detects which one is fitted. |
| PMU | SY6970 charger (I2C `0x6A`). The firmware raises its input current limit at boot, because the 0.53 A default causes brownouts and phantom touches. |
| Backlight | PT4103 driver, PWM on GPIO1 |

The board used for development reports touch `AXS15231B`, PMU present (`stats` console command).

## Pins used on the board

| Function | GPIO | Notes |
|---|---|---|
| LCD QSPI CS / SCK / D0 / D1 / D2 / D3 | 12 / 17 / 13 / 18 / 21 / 14 | 32 MHz (40 MHz corrupts the image on this panel) |
| LCD reset (also resets the AXS touch) | 16 | |
| Backlight PWM | 1 | 2 kHz |
| Touch I2C SDA / SCL | 15 / 10 | Shared with the PMU and the QWIIC port P9 |
| Touch interrupt | 11 | |
| CST3530 touch reset (new boards only) | 2 | Battery ADC on older boards; only driven if the AXS touch is absent |
| BOOT button | 0 | |

## Pins for the desk and LEDs

The defaults below are set in `idf.py menuconfig` → **Smart Desk Controller**. All of them are on the 30-pin P5 header.

| Function | Default GPIO | P5 header pin |
|---|---|---|
| Desk UART TX → RJ45 pin 6 | **6** | 15 |
| Desk UART RX ← RJ45 pin 5 | **7** | 13 |
| Desk wake → RJ45 pin 4 | **5** | 17 |
| LED strip data | **48** | 7 |
| GND | - | 1, 3, 2, 4, 24, 26 |
| 3V3 | - | 28, 30 |

Avoid these GPIOs:
- **3, 45 and 46:** strapping pins.
- **38–41:** used by the SD card on the SD-slot version.
- **10 and 15:** the shared I2C bus.
- **43 and 44:** UART0. The ROM bootloader prints on 43 at boot.

## Wiring diagram

```
 T-Display-S3-Long (P5 header)                        Desk control box RJ45
 ─────────────────────────────                        ─────────────────────
                                    ┌──────────────┐
 GPIO6  (TX) ───────────────────────┤ LV1      HV1 ├── pin 6  (box RX)
 GPIO7  (RX) ───────────────────────┤ LV2      HV2 ├── pin 5  (box TX)
 GPIO5  (wake) ──┬──────────────────┤ LV3      HV3 ├── pin 4  ("PIN 20")
 3V3 ──[10 kΩ]───┘  (keeps "handset present" asserted while the ESP32 resets)
 3V3 ───────────────────────────────┤ LV        HV ├── pin 8  (+5 V)
 GND ───────────────────────────────┤ GND      GND ├── pin 7  (GND)
                                    └──────────────┘
                                 4-channel BSS138 level shifter

 GPIO48 ── 330 Ω ──────────────── LED strip DIN
 GND ──────────────────────────── LED strip GND ──┐
                                                  ├── 5 V PSU GND
 5 V PSU + ───────────────────────── LED strip 5V ┘   (size for the strip:
                                                        up to 60 mA per LED)
 USB-C ← 5 V USB supply (powers the board)
```

**Minimum RX-only alternative.** If you skip the shifter, still protect the RX pin:

```
 RJ45 pin 5 ──[10 kΩ]──┬── GPIO7
                       └──[20 kΩ]── GND
 RJ45 pin 6 ─────────────── GPIO6     (3.3 V drive works on most E7-family boxes)
 RJ45 pin 4 ─────────────── GPIO5  ──[10 kΩ]── 3V3
 RJ45 pin 7 ─────────────── GND
```

## Power

- **Board:** power it from USB-C. The desk's 5 V rail (RJ45 pin 8) sags under Wi-Fi transmit bursts and has caused resets in other builds.
- **Grounds:** tie the desk, board and LED supply grounds together.
- **LED strip:** give it its own 5 V supply. `CONFIG_LED_STRIP_MAX_BRIGHTNESS` (default 60 %) caps current draw.
- **Battery:** optional. The board runs fine without one.

## Mounting

- **Orientation:** the UI is landscape (640×180). If the panel is mounted upside down, turn on **Settings → Display → Rotate 180°**, which flips both the image and touch.
- **Phantom touches:** the AXS15231B touch layer is known for them.
  - **What testing found on this board:** they happened about every 1.5–5 minutes, lasted about 20–60 ms, and occurred **only while the backlight was dimmed**. That is low PWM duty; there were none in 10 minutes at full brightness.
  - **What the firmware does:**
    - debounces touches (2 consistent samples)
    - makes Up/Down wait for 150 ms of continuous press
    - ignores preset taps shorter than 60 ms
    - lets the first touch on a dimmed screen only wake it
  - **Diagnostics:** every accepted press is logged with its duration and raw controller bytes (`touch up: ... held N ms`) to help diagnose it.
  - **Cabling:** keep the flex cable away from the LED strip data line and the desk motor wiring.
