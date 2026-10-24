// Theme palettes. Add a new entry to kPalettes to make a theme selectable;
// the settings screen and Home Assistant pick it up automatically.
#include "theme.h"

namespace ui {

namespace {

inline lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

const Palette kPalettes[] = {
    {
        .name = "Midnight",
        .dark = true,
        .bg = hex(0x0A0C10),
        .surface = hex(0x161A21),
        .surface_2 = hex(0x222833),
        .border = hex(0x2B313C),
        .text = hex(0xEEF1F6),
        .text_muted = hex(0x9AA3B2),
        .text_faint = hex(0x5E6776),
        .accent = hex(0x4F8CFF),
        .on_accent = hex(0xFFFFFF),
        .positive = hex(0x34D399),
        .warning = hex(0xFBBF24),
        .danger = hex(0xF87171),
    },
    {
        .name = "Graphite",
        .dark = true,
        .bg = hex(0x0B0B0B),
        .surface = hex(0x181818),
        .surface_2 = hex(0x252525),
        .border = hex(0x303030),
        .text = hex(0xF5F5F5),
        .text_muted = hex(0xA3A3A3),
        .text_faint = hex(0x606060),
        .accent = hex(0xF5A524),
        .on_accent = hex(0x151007),
        .positive = hex(0x4ADE80),
        .warning = hex(0xFACC15),
        .danger = hex(0xF87171),
    },
    {
        .name = "Forest",
        .dark = true,
        .bg = hex(0x070F0B),
        .surface = hex(0x101C16),
        .surface_2 = hex(0x192A21),
        .border = hex(0x23362C),
        .text = hex(0xE8F4ED),
        .text_muted = hex(0x93AD9F),
        .text_faint = hex(0x566A5F),
        .accent = hex(0x3DD68C),
        .on_accent = hex(0x03130B),
        .positive = hex(0x3DD68C),
        .warning = hex(0xF2C94C),
        .danger = hex(0xFF7B72),
    },
    {
        .name = "Ember",
        .dark = true,
        .bg = hex(0x100A08),
        .surface = hex(0x1D1411),
        .surface_2 = hex(0x2A1D18),
        .border = hex(0x382821),
        .text = hex(0xFBEFEA),
        .text_muted = hex(0xBCA197),
        .text_faint = hex(0x705B53),
        .accent = hex(0xFF7A45),
        .on_accent = hex(0x1A0A04),
        .positive = hex(0x6EE7B7),
        .warning = hex(0xFBBF24),
        .danger = hex(0xFF5A5F),
    },
    {
        .name = "Violet",
        .dark = true,
        .bg = hex(0x0C0A13),
        .surface = hex(0x17131F),
        .surface_2 = hex(0x221C2E),
        .border = hex(0x2F2740),
        .text = hex(0xF1EDF8),
        .text_muted = hex(0xA89EBB),
        .text_faint = hex(0x635A74),
        .accent = hex(0xA78BFA),
        .on_accent = hex(0x140B2A),
        .positive = hex(0x5EEAD4),
        .warning = hex(0xFCD34D),
        .danger = hex(0xFB7185),
    },
    {
        .name = "OLED",
        .dark = true,
        .bg = hex(0x000000),
        .surface = hex(0x0F0F0F),
        .surface_2 = hex(0x1C1C1C),
        .border = hex(0x262626),
        .text = hex(0xFFFFFF),
        .text_muted = hex(0x9E9E9E),
        .text_faint = hex(0x575757),
        .accent = hex(0x22D3EE),
        .on_accent = hex(0x001317),
        .positive = hex(0x4ADE80),
        .warning = hex(0xFACC15),
        .danger = hex(0xF87171),
    },
    {
        .name = "Daylight",
        .dark = false,
        .bg = hex(0xE9EDF2),
        .surface = hex(0xFFFFFF),
        .surface_2 = hex(0xDDE3EA),
        .border = hex(0xCFD6DF),
        .text = hex(0x10141A),
        .text_muted = hex(0x566170),
        .text_faint = hex(0x97A1AE),
        .accent = hex(0x2563EB),
        .on_accent = hex(0xFFFFFF),
        .positive = hex(0x059669),
        .warning = hex(0xD97706),
        .danger = hex(0xDC2626),
    },
};

}  // namespace

int palette_count() { return static_cast<int>(sizeof(kPalettes) / sizeof(kPalettes[0])); }

const Palette &palette(int index)
{
    if (index < 0 || index >= palette_count()) {
        index = 0;
    }
    return kPalettes[index];
}

}  // namespace ui
