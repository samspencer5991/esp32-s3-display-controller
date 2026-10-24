#include "desk/protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace desk {

uint16_t preset_key(int index)
{
    static const uint16_t keys[] = {KeyPreset1, KeyPreset2, KeyPreset3, KeyPreset4};
    return (index >= 0 && index < 4) ? keys[index] : static_cast<uint16_t>(KeyNone);
}

uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

size_t encode_frame(const Frame &f, uint8_t *out)
{
    size_t n = 0;
    out[n++] = kFrameStart;
    out[n++] = static_cast<uint8_t>(f.len + 4);
    out[n++] = f.type;
    memcpy(out + n, f.payload, f.len);
    n += f.len;
    uint16_t crc = crc16_modbus(out + 1, n - 1);
    out[n++] = static_cast<uint8_t>(crc >> 8);
    out[n++] = static_cast<uint8_t>(crc & 0xFF);
    out[n++] = kFrameEnd;
    return n;
}

size_t encode_keys(uint16_t keys, uint8_t *out)
{
    Frame f;
    f.type = kTypeKeys;
    f.len = 2;
    f.payload[0] = static_cast<uint8_t>(keys & 0xFF);
    f.payload[1] = static_cast<uint8_t>(keys >> 8);
    return encode_frame(f, out);
}

FrameParser::Result FrameParser::fail()
{
    // Resynchronise: restart from the next start byte inside the buffer, if any.
    size_t next = 1;
    while (next < pos_ && buf_[next] != kFrameStart) {
        next++;
    }
    const size_t remaining = pos_ - next;
    memmove(buf_, buf_ + next, remaining);
    pos_ = 0;
    // Re-feed the remainder so a frame starting inside the rejected bytes is
    // not lost. (A *complete* frame hidden inside a rejected one is dropped;
    // the control box repeats its frames continuously.)
    uint8_t tmp[kMaxFrame];
    memcpy(tmp, buf_, remaining);
    for (size_t i = 0; i < remaining; i++) {
        feed(tmp[i]);
    }
    return Result::Error;
}

FrameParser::Result FrameParser::feed(uint8_t byte)
{
    if (pos_ == 0) {
        if (byte == kFrameStart) {
            buf_[pos_++] = byte;
        }
        return Result::None;
    }

    buf_[pos_++] = byte;
    if (pos_ == 2 && (byte < 4 || byte > kMaxPayload + 4)) {
        return fail();
    }
    if (pos_ < 2) {
        return Result::None;
    }

    const size_t total = static_cast<size_t>(buf_[1]) + 2;  // start + LEN + everything LEN counts
    if (pos_ < total) {
        return Result::None;
    }
    if (buf_[total - 1] != kFrameEnd) {
        return fail();
    }
    const size_t crc_pos = total - 3;
    const uint16_t expected = static_cast<uint16_t>((buf_[crc_pos] << 8) | buf_[crc_pos + 1]);
    if (crc16_modbus(buf_ + 1, crc_pos - 1) != expected) {
        return fail();
    }

    frame_.type = buf_[2];
    frame_.len = static_cast<uint8_t>(buf_[1] - 4);
    memcpy(frame_.payload, buf_ + 3, frame_.len);
    memcpy(raw_, buf_, total);
    raw_len_ = total;
    pos_ = 0;
    return Result::Frame;
}

// --- 7-segment -------------------------------------------------------------

namespace {

constexpr uint8_t kDigitSegments[10] = {
    0x3F,  // 0
    0x06,  // 1
    0x5B,  // 2
    0x4F,  // 3
    0x66,  // 4
    0x6D,  // 5
    0x7D,  // 6
    0x07,  // 7
    0x7F,  // 8
    0x6F,  // 9
};

struct CharSeg {
    char c;
    uint8_t seg;
};

// Glyphs the control box shows besides digits (LoC, S-1, A-3, E01, ASr, rSt...).
constexpr CharSeg kLetters[] = {
    {'A', 0x77}, {'b', 0x7C}, {'C', 0x39}, {'c', 0x58}, {'d', 0x5E}, {'E', 0x79}, {'F', 0x71},
    {'H', 0x76}, {'h', 0x74}, {'L', 0x38}, {'n', 0x54}, {'o', 0x5C}, {'P', 0x73}, {'r', 0x50},
    {'t', 0x78}, {'U', 0x3E}, {'u', 0x1C}, {'y', 0x6E}, {'-', 0x40}, {'_', 0x08}, {' ', 0x00},
    {'N', 0x37}, {'r', 0x31}, {'=', 0x09},
    // Encode-only aliases (decode prefers the digit with the same pattern).
    {'S', 0x6D}, {'O', 0x3F},
};

}  // namespace

int segment_to_digit(uint8_t seg)
{
    seg &= 0x7F;
    for (int d = 0; d < 10; d++) {
        if (kDigitSegments[d] == seg) {
            return d;
        }
    }
    // Some displays render 7 and 9 with the extra "tail" segment.
    if (seg == 0x27) return 7;
    if (seg == 0x67) return 9;
    return -1;
}

char segment_to_char(uint8_t seg)
{
    seg &= 0x7F;
    int d = segment_to_digit(seg);
    if (d >= 0) {
        return static_cast<char>('0' + d);
    }
    for (const auto &l : kLetters) {
        if (l.seg == seg) {
            return l.c;
        }
    }
    return '?';
}

uint8_t digit_to_segment(int digit)
{
    return (digit >= 0 && digit <= 9) ? kDigitSegments[digit] : 0;
}

uint8_t char_to_segment(char c)
{
    if (c >= '0' && c <= '9') {
        return kDigitSegments[c - '0'];
    }
    for (const auto &l : kLetters) {
        if (l.c == c) {
            return l.seg;
        }
    }
    // Fall back to case-insensitive match.
    for (const auto &l : kLetters) {
        if ((l.c | 0x20) == (c | 0x20)) {
            return l.seg;
        }
    }
    return 0;
}

DisplayReading decode_display(const uint8_t seg[3])
{
    DisplayReading r;
    const uint8_t s0 = seg[0] & 0x7F, s1 = seg[1] & 0x7F, s2 = seg[2] & 0x7F;

    if ((s0 | s1 | s2) == 0) {
        r.kind = DisplayKind::Blank;
        return r;
    }

    const int d0 = segment_to_digit(s0);
    const int d1 = segment_to_digit(s1);
    const int d2 = segment_to_digit(s2);

    // Height: three digits (leading digit may be blank), decimal point on the
    // middle digit means one decimal place.
    if ((d0 >= 0 || s0 == 0) && d1 >= 0 && d2 >= 0) {
        const int lead = s0 == 0 ? 0 : d0;
        r.kind = DisplayKind::Height;
        if (seg[1] & kSegDecimalPoint) {
            r.value = lead * 10.0f + d1 + d2 / 10.0f;
            snprintf(r.text, sizeof(r.text), "%d.%d", lead * 10 + d1, d2);
        } else {
            r.value = static_cast<float>(lead * 100 + d1 * 10 + d2);
            snprintf(r.text, sizeof(r.text), "%d", lead * 100 + d1 * 10 + d2);
        }
        return r;
    }

    for (int i = 0; i < 3; i++) {
        r.text[i] = segment_to_char(seg[i]);
    }
    r.text[3] = '\0';

    if (s0 == 0x38 && s1 == 0x5C && s2 == 0x39) {  // "LoC"
        r.kind = DisplayKind::Locked;
        snprintf(r.text, sizeof(r.text), "LoC");
    } else if (s0 == 0x6D && s1 == 0x40) {  // "S-" / "S-n"
        r.kind = DisplayKind::SaveMode;
        r.arg = segment_to_digit(s2);
        snprintf(r.text, sizeof(r.text), r.arg >= 0 ? "S-%d" : "S-", r.arg);
    } else if (s0 == 0x77 && s1 == 0x40) {  // "A-n"
        r.kind = DisplayKind::Sensitivity;
        r.arg = segment_to_digit(s2);
        snprintf(r.text, sizeof(r.text), "A-%d", r.arg >= 0 ? r.arg : 0);
    } else if ((s0 == 0x77 && s1 == 0x6D && s2 == 0x31) || (s0 == 0x50 && s1 == 0x6D && s2 == 0x78)) {
        r.kind = DisplayKind::Reset;  // "ASr" / "rSt"
        snprintf(r.text, sizeof(r.text), s0 == 0x77 ? "ASr" : "rSt");
    } else {
        r.kind = DisplayKind::Error;  // "E01", "E02", ... or unknown glyphs
    }
    return r;
}

void encode_height_display(float value, uint8_t seg[3])
{
    if (value < 100.0f) {
        int tenths = static_cast<int>(std::lround(value * 10.0f));
        if (tenths > 999) tenths = 999;
        if (tenths < 0) tenths = 0;
        seg[0] = digit_to_segment(tenths / 100);
        seg[1] = digit_to_segment((tenths / 10) % 10) | kSegDecimalPoint;
        seg[2] = digit_to_segment(tenths % 10);
    } else {
        int whole = static_cast<int>(std::lround(value));
        if (whole > 999) whole = 999;
        seg[0] = digit_to_segment(whole / 100);
        seg[1] = digit_to_segment((whole / 10) % 10);
        seg[2] = digit_to_segment(whole % 10);
    }
}

void encode_text_display(const char *text3, uint8_t seg[3])
{
    for (int i = 0; i < 3; i++) {
        seg[i] = text3[i] ? char_to_segment(text3[i]) : 0;
    }
}

}  // namespace desk
