# Flexispot E7 Pro / LoctekMotion desk protocol

This controller replaces the desk's handset. It connects to the control box's RJ45 handset port and speaks the LoctekMotion serial protocol. This page covers:

- what the community has established about the protocol
- which choices this firmware makes
- what is still unverified
- how to bring the desk up safely

> **Status:** everything here comes from published research and captures. The implementation has been tested against a simulated control box that follows those captures. It has **not** yet been tested against a real E7 Pro. Follow the [first-connection checklist](#first-connection-checklist) the first time.

## Hardware

### Handset and control box

- **Handset:** the E7 Pro keypad is the **HS13M-1C0**.
- **Control box:** the model varies by production batch:
  - E7 Pro Plus: `CB38M2L(IB)-1`
  - 2026 E7 Pro: `CB38M2M(IB)-4`
- **Compatibility:** community projects confirm the RJ45 protocol works on the E7 Pro, the E7 Pro Plus and the 2026 edition.

### RJ45 pinout

Pins use T568B numbering: clip facing away from you, contacts up, pin 1 on the left.

| Pin | Colour (T568B) | Signal | Connect to |
|---|---|---|---|
| 8 | brown | +5 V from control box | Optional supply (weak, see below) |
| 7 | white/brown | GND | ESP32 GND |
| 6 | green | Box RX (handset TX) | `CONFIG_DESK_UART_TX_GPIO` (default **GPIO6**) |
| 5 | white/blue | Box TX (handset RX) | `CONFIG_DESK_UART_RX_GPIO` (default **GPIO7**) **through a level shifter/divider** |
| 4 | blue | "PIN 20", wake / handset present | `CONFIG_DESK_WAKE_GPIO` (default **GPIO5**) |
| 3 | - | unused | leave open |
| 2 | - | SWIM (box MCU programming) | **leave open** |
| 1 | - | RESET | **leave open** |

⚠️ **Published tables disagree on pins 5 and 6.**
- **Before connecting anything:** check with a meter that pin 8 is +5 V and pin 7 is GND.
- **Before connecting RX:** check that pin 5 idles high, which marks the box's TX line.

### Voltage levels

**Box TX (pin 5):** measurements disagree. Some show 5 V logic, others about 3.3 V. The ESP32-S3 is **not 5 V tolerant**, so put a divider on RX (e.g. 10 kΩ series and 20 kΩ to GND) or use a BSS138 level shifter.

**Handset TX (pin 6) and wake (pin 4):**
- Several E7-family builds drive these at 3.3 V and it works.
- One capture (not an E7) found 3.3 V TX unreliable while the stock keypad was also on the bus.
- A bidirectional shifter is the safe choice.

**Power:**
- **The 5 V rail on pin 8 is weak:** it gives roughly 200 mA before it sags, and Wi-Fi bursts cause brownouts.
- **Recommended:** power the T-Display-S3-Long from its own USB supply and share only GND with the desk.

See [hardware.md](hardware.md) for the full wiring diagram.

## Serial settings

9600 baud, 8 data bits, no parity, 1 stop bit, no flow control, non-inverted TTL.

## Frame format

```
0x9B | LEN | TYPE | PAYLOAD ... | CRC_hi | CRC_lo | 0x9D
```

- **`LEN`** is the number of bytes after `LEN`, including the `0x9D` end byte, so `LEN = payload + 4`. Total frame length is `LEN + 2`.
- **`CRC`** is CRC-16/MODBUS (init `0xFFFF`, reflected polynomial `0xA001`) computed over `LEN, TYPE, PAYLOAD`. It is sent **high byte first**, the opposite of normal Modbus RTU.
  - Example: `06 02 00 00` has CRC `0x6CA1`, sent as `6C A1`.
  - This was verified against more than 280 captured frames.

Implementation: [`components/desk/protocol.cpp`](../components/desk/protocol.cpp). The unit tests in [`test/host/test_desk.cpp`](../test/host/test_desk.cpp) check the encoder against the published frames byte for byte.

### Handset → box: keys (type `0x02`)

The payload is a two-byte key bitmask, little endian (byte 0 holds bits 0–7). Combinations are bitwise OR.

| Key | Mask | Frame | E7 Pro |
|---|---|---|---|
| none ("wake") | `0x0000` | `9B 06 02 00 00 6C A1 9D` | ✓ |
| Up | `0x0001` | `9B 06 02 01 00 FC A0 9D` | ✓ |
| Down | `0x0002` | `9B 06 02 02 00 0C A0 9D` | ✓ |
| Preset 1 | `0x0004` | `9B 06 02 04 00 AC A3 9D` | ✓ |
| Preset 2 | `0x0008` | `9B 06 02 08 00 AC A6 9D` | ✓ |
| Preset 3 | `0x0010` | `9B 06 02 10 00 AC AC 9D` | ✓ |
| M (memory) | `0x0020` | `9B 06 02 20 00 AC B8 9D` | ✓ |
| Alarm | `0x0040` | `9B 06 02 40 00 AC 90 9D` | not on the E7 Pro keypad; unverified |
| Preset 4 | `0x0100` | `9B 06 02 00 01 AC 60 9D` | ✓ |

**Key combinations (from other models):**
- Up+Down held for 5 s enters anti-collision sensitivity setup; the display shows `A-n`.
- M+Preset 3 held for 3 s is reported as a factory reset. **This firmware never sends combinations** except through the console's `desk keys` command.

### Box → handset

| Type | Frame | Meaning |
|---|---|---|
| `0x11` | `9B 04 11 7C C3 9D` | Key poll: answer with the current key mask |
| `0x12` | `9B 07 12 d0 d1 d2 CRC 9D` | Display contents, 3 × 7-segment digits (`00 00 00` = display off) |
| `0x13` | `9B 04 13 BD 42 9D` | Going to sleep |
| `0x14` | `9B 04 14 7F 03 9D` | Buzzer |
| `0x15` | `9B 04 15 BF C2 9D` | Keep-alive |

Some 4-motor boxes (E7Q) send 10-byte display frames. The parser accepts payloads up to 12 bytes, and only the first three are decoded.

### 7-segment decoding

- **Bit layout:** bit 0 = segment a (top), bit 6 = g (middle), bit 7 = decimal point.
- **Digits:** `3F 06 5B 4F 66 6D 7D 07 7F 6F` for 0–9.
- **Heights:**
  - With a decimal point on the middle digit, the value is `XX.X` (below 100 cm).
  - Without one, it is a whole number (100 cm and above).
  - In inch mode the box shows `XX.X`.

Special displays the firmware recognises:

| Display | Bytes | Meaning | UI behaviour |
|---|---|---|---|
| `LoC` | `38 5C 39` | Control box child lock engaged | "Desk locked" plus a dialog offering to unlock (holds M for 5 s) |
| `S-` / `S-n` | `6D 40 xx` | M armed / preset n stored | "Saving position" |
| `A-n` | `77 40 xx` | Sensitivity setting | shown in diagnostics |
| `ASr` / `rSt` | `77 6D 31` / `50 6D 78` | Box needs a reset run | Dialog: hold Down until the desk bottoms out |
| `E01`, `E02`, … | `79 3F 06` … | Error | Error dialog |

## Timing: what the firmware does

The key behaviour lives in [`components/desk/controller.cpp`](../components/desk/controller.cpp), and every value is in `desk::Timing`.

| Behaviour | Implementation | Source |
|---|---|---|
| Key transmission | Answer every `0x11` poll immediately with the current mask (`00 00` when idle). If no polls are seen for 400 ms, send unsolicited frames every 108 ms while a key is down. | Boxes poll every ~40 ms (E7 Pro); the 108 ms unsolicited rate is used by most community projects |
| Hold Up/Down | Mask held while the UI refreshes it; **releases automatically 400 ms after the last refresh** | Fail-safe, so a stuck UI never leaves the desk driving |
| Timed presses | A press's duration counts from its **first frame actually sent**, and every press lasts at least 3 frames. A stalled task can delay a key but can never shorten or skip it. | Found in code review: a skipped M turned "save" into "move" |
| Go to preset | Preset key pressed for 500 ms. A preset request during an automatic move is treated as **stop**, as on the stock handset. | 150 ms to 1 s works on captured boxes |
| Stop a preset move | 80 ms tap (at least 3 frames) of the key opposite the travel direction, sent only if the preset key actually went out. If it never went out, the pending press is just cancelled. A desk coasting after a manual move gets no tap. | `00 00` does **not** cancel a preset move; a short Up/Down tap does |
| Save preset | M 300 ms → released 400 ms → Preset N 300 ms. Refused while the desk moves. | M arms "S-" for about 3–4 s |
| Preset overwrite guard | Preset keys and Up/Down are **refused** while the box may still be armed by M: while M is queued or being sent, while the display shows `S-`, or for 5 s after M without a following preset key | The next preset key would *store* instead of move |
| Control box lock | M held 5.5 s; the box toggles `LoC` | E7 Pro manual: hold M about 5 s |
| Wake pin | Held **high** permanently. A 150 ms low pulse turns the display on at boot, and before a key press if the display is blank (followed by a 600 ms settle) | E7 Pro capture: the stock handset holds pin 4 at 5 V; a low→high edge lights the LCD |
| Link state | Offline after 2 s without a valid frame | - |
| Motion detection | Height changing between display frames; idle after 800 ms without change | Display frames arrive ~14/s while awake |

**Heights while the display is off:**
- **What the box sends:** blank frames (`00 00 00`).
- **What the firmware does:** keeps the last known height. The desk can't move without this controller, and the last height is saved to NVS once the desk settles, so it survives reboots.

### Hazard: never drop off the bus mid-move

If the handset disappears during a preset move, the box can enter its reset state (`ASr`/`rSt`, where only Down works). To guard against this:
- **Restart** (UI, console): keeps sending stop until the desk is idle (up to 20 s, otherwise it refuses), holds the wake pin through the reboot, then restarts.
- **Demo mode** can't be switched while the desk moves, is travelling or is sending keys, so the real desk is never left without a controller.
- **Boot:** the wake line is driven high first thing at boot. Every ESP32 reset still floats it for about half a second, so fit a 10 kΩ pull-up to 3.3 V on it (see [hardware.md](hardware.md)).

## Open questions to verify on the real desk

1. **Pinout:** confirm which of pins 5 and 6 is TX/RX, and the voltage on pin 5.
2. **Polling:** does the E7 Pro box poll (`0x11`) with this handset connected? Check `sniff`: `polled` mode shows RX `9B 04 11 …` frames about every 40 ms.
3. **Wake:** does holding pin 4 high keep display frames flowing, and does the low pulse light the display?
4. **Stop tap:** does the brief Up/Down tap stop a preset move cleanly, and how far does the desk coast?
5. **Preset 3/4:** are `0x0010` and `0x0100` in that order on this box? (One other panel reported them swapped.)
6. **Units:** does the box report cm? If it's set to inches, enable `CONFIG_DESK_REPORTS_INCHES`.
7. **Lock state:** does `LoC` only appear after a key press while locked? The firmware treats `LoC` as locked until the desk moves again.

## First-connection checklist

1. **Meter the port with the desk plugged in:**
   - pin 8 to pin 7 should read +5 V
   - pin 5 should idle high (note whether that is 3.3 V or 5 V)
   - pin 4 should read low with nothing connected
2. **Wire it up** as in [hardware.md](hardware.md), with a divider or shifter on RX. Power the board from USB.
3. **Leave demo mode on for now** (Settings → Desk). Open a serial console and run `sniff 20`.
   - With demo mode on you'll see the simulated box.
   - Turn demo mode off (`desk demo off`) and run `sniff 20` again. You should see real `RX 9B 04 11 …` polls and `RX 9B 07 12 …` display frames.
   - If nothing arrives, swap the TX/RX GPIOs (`idf.py menuconfig` → Smart Desk Controller) or check the wake wiring.
4. **Check the reported state:** `state` should show `link=online`, and `display` should match the desk.
5. **Test motion:** stand clear and run `desk up 500`, then `desk down 500`. The desk should move briefly and stop.
6. **Test presets:** run `desk preset 1` and stop it with `desk stop`, then let one run to completion.
7. **Save from the screen:** hold a preset button on the screen for about 2 s to save. `sniff` should show `M` then `Preset` key frames, and the box shows `S-n`.
8. **Report:** if anything behaves differently, capture `sniff 30 all` output. The `desk keys <hex> [ms]` command sends arbitrary masks for experiments.

## Sources

- iMicknl, [LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT): the original research, pinouts, ESPHome packages, and 240+ issues with captures.
- Home Assistant community, [Turn your LoctekMotion/Flexispot desk into a smart desk](https://community.home-assistant.io/t/turn-your-loctekmotion-flexispot-standing-desk-into-a-smart-desk/294650).
- Ideal Reality, [E7 Pro handset replacement with an ESP32-S3](https://ideal-reality.com/post/000d) and its [ESP-IDF driver](https://github.com/Hiroki-Kawakami/idf-components/blob/main/flexispot/flexispot.c): poll/response model, wake pin behaviour, `LoC` handling on the E7 Pro.
- lapelotanodobla, [FlexiSpot_MitM](https://github.com/lapelotanodobla/FlexiSpot_MitM): man-in-the-middle captures, timing, stop-tap finding, voltage measurements.
- dimitri-vs, [flexispot-esphome](https://github.com/dimitri-vs/flexispot-esphome): E7 Pro Plus wake timing, M/S- behaviour, display sleep timing.
- muxa, [esphome-loctekmotion-desk](https://github.com/muxa/esphome-loctekmotion-desk): 7-segment tables.
- zaxbux, [LoctekMotion-reng](https://github.com/zaxbux/LoctekMotion-reng); LEXYGO, [FlexiSmart](https://github.com/LEXYGO/FlexiSmart) (2026 E7 Pro); lexbrugman, zjean and mattanapol projects: measurements, coast distances and lock handling.
- Flexispot E7 Pro manual: child lock (hold M for 5 s) and 63.5–128.5 cm height range.
