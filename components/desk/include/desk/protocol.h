// LoctekMotion control box protocol (Flexispot E-series handset port).
//
// Frame layout (both directions), 9600 8N1:
//
//   0x9B | LEN | TYPE | PAYLOAD ... | CRC_hi | CRC_lo | 0x9D
//
//   LEN  number of bytes after LEN, including the 0x9D end byte
//        (LEN = payload_len + 4; total frame length = LEN + 2).
//   CRC  CRC-16/MODBUS (init 0xFFFF, poly 0xA001) over [LEN, TYPE, PAYLOAD],
//        transmitted high byte first (unlike Modbus RTU).
//
// Control box -> handset:
//   0x11  key poll; the handset answers every poll with its key mask
//   0x12  display: 3 x 7-segment bytes (all zero = display off)
//   0x13  going to sleep
//   0x14  buzzer
//   0x15  keep-alive
// Handset -> control box:
//   0x02  key mask, 2 bytes little-endian (00 00 = no key)
//
// See docs/desk-protocol.md for sources, timing and open questions.
#pragma once

#include <cstddef>
#include <cstdint>

namespace desk {

constexpr uint8_t kFrameStart = 0x9B;
constexpr uint8_t kFrameEnd = 0x9D;
constexpr size_t kMaxPayload = 12;
constexpr size_t kMaxFrame = kMaxPayload + 6;

constexpr uint8_t kTypeKeys = 0x02;
constexpr uint8_t kTypePoll = 0x11;
constexpr uint8_t kTypeDisplay = 0x12;
constexpr uint8_t kTypeSleep = 0x13;
constexpr uint8_t kTypeBuzzer = 0x14;
constexpr uint8_t kTypeKeepAlive = 0x15;

// Handset key bits (payload byte0 = bits 0..7, byte1 = bits 8..15).
enum Key : uint16_t {
    KeyNone = 0,
    KeyUp = 0x0001,
    KeyDown = 0x0002,
    KeyPreset1 = 0x0004,
    KeyPreset2 = 0x0008,
    KeyPreset3 = 0x0010,
    KeyMemory = 0x0020,
    KeyAlarm = 0x0040,  // not present on the E7 Pro handset; unverified
    KeyPreset4 = 0x0100,
};

uint16_t preset_key(int index);  // 0..3 -> KeyPresetN, KeyNone otherwise

uint16_t crc16_modbus(const uint8_t *data, size_t len);

struct Frame {
    uint8_t type = 0;
    uint8_t len = 0;  // payload length
    uint8_t payload[kMaxPayload] = {};
};

// Encodes a frame into `out` (must hold kMaxFrame bytes). Returns bytes written.
size_t encode_frame(const Frame &f, uint8_t *out);
size_t encode_keys(uint16_t keys, uint8_t *out);

// Incremental byte-stream parser. Resynchronises on the start byte and
// rejects frames with a bad length, CRC or end byte.
class FrameParser {
public:
    enum class Result { None, Frame, Error };

    Result feed(uint8_t byte);
    const Frame &frame() const { return frame_; }
    // Raw bytes of the last completed frame, for diagnostics.
    const uint8_t *raw() const { return raw_; }
    size_t raw_len() const { return raw_len_; }

private:
    Result fail();

    uint8_t buf_[kMaxFrame] = {};
    size_t pos_ = 0;
    uint8_t raw_[kMaxFrame] = {};
    size_t raw_len_ = 0;
    Frame frame_;
};

// --- 7-segment display ------------------------------------------------------
//
// Segment bits: bit0=a (top) bit1=b bit2=c bit3=d bit4=e bit5=f bit6=g, bit7=dp.

constexpr uint8_t kSegDecimalPoint = 0x80;

// Returns the digit 0..9 for a segment pattern (dp ignored), or -1.
int segment_to_digit(uint8_t seg);
// Best-effort character for a segment pattern ('0'..'9', letters, '-', ' ', '?').
char segment_to_char(uint8_t seg);
uint8_t digit_to_segment(int digit);
uint8_t char_to_segment(char c);

enum class DisplayKind : uint8_t {
    Blank,        // display off (00 00 00)
    Height,       // numeric height
    Locked,       // "LoC" - child lock engaged on the control box
    SaveMode,     // "S-" / "S-n" - memory key armed / preset stored
    Sensitivity,  // "A-n" - anti-collision sensitivity setting
    Reset,        // "ASr" / "rSt" - control box needs a reset run
    Error,        // "E01".. or anything unrecognised
};

struct DisplayReading {
    DisplayKind kind = DisplayKind::Blank;
    float value = 0;    // height, in whatever unit the control box uses
    char text[8] = {};  // printable rendering, e.g. "72.5", "112", "LoC", "E01"
    int arg = -1;       // digit for S-n / A-n
};

DisplayReading decode_display(const uint8_t seg[3]);

// Encodes a height the way the control box shows it: one decimal below 100,
// whole units at or above 100. Used by the simulated control box.
void encode_height_display(float value, uint8_t seg[3]);
void encode_text_display(const char *text3, uint8_t seg[3]);

}  // namespace desk
