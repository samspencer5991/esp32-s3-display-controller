#!/usr/bin/env python3
"""Generate the LVGL bitmap fonts used by the UI.

Pipeline per font: variable TTF -> static instance (fontTools instancer)
-> optional OpenType feature freeze (e.g. tabular digits) -> lv_font_conv.

Requirements (see README.md, "Regenerating fonts"):
    python3 -m venv tools/.venv && tools/.venv/bin/pip install fonttools opentype-feature-freezer
    node / npx (lv_font_conv is fetched on demand)

Source fonts are downloaded into tools/fonts/src on first run.

Usage:  tools/.venv/bin/python tools/fonts/generate_fonts.py
"""

from __future__ import annotations

import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SRC = HERE / "src"
BUILD = HERE / "build"
OUT = ROOT / "components" / "ui" / "fonts"
BIN = Path(sys.executable).parent

DOWNLOADS = {
    "Inter.ttf": "https://github.com/google/fonts/raw/main/ofl/inter/Inter%5Bopsz,wght%5D.ttf",
    "Inter-OFL.txt": "https://github.com/google/fonts/raw/main/ofl/inter/OFL.txt",
    "MaterialSymbolsRounded.ttf": "https://github.com/google/material-design-icons/raw/master/variablefont/"
    "MaterialSymbolsRounded%5BFILL,GRAD,opsz,wght%5D.ttf",
    "MaterialSymbols-LICENSE.txt": "https://github.com/google/material-design-icons/raw/master/LICENSE",
}

TEXT_RANGE = "0x20-0x7E"
TEXT_EXTRA = "°·–—…×±→←↑↓"

# Icons: name -> Material Symbols codepoint. The generated header exposes
# UI_ICON_<NAME> as a UTF-8 string literal usable in any label.
ICONS = {
    "lock": 0xE899,
    "lock_open": 0xE898,
    "settings": 0xE8B8,
    "lightbulb": 0xE90F,
    "light_off": 0xE9B8,
    "arrow_up": 0xE316,  # keyboard_arrow_up
    "arrow_down": 0xE313,  # keyboard_arrow_down
    "wifi": 0xE63E,
    "wifi_off": 0xE648,
    "wifi_1": 0xE4CA,
    "wifi_2": 0xE4D9,
    "chair": 0xEFED,
    "stand": 0xE92C,  # accessibility_new
    "timer": 0xE425,
    "alarm": 0xE855,
    "palette": 0xE40A,
    "back": 0xE5C4,
    "check": 0xE668,
    "close": 0xE5CD,
    "stop": 0xE047,
    "notifications": 0xE7F5,
    "tune": 0xE429,
    "info": 0xE88E,
    "light_mode": 0xE518,
    "dark_mode": 0xE51C,
    "straighten": 0xE41C,
    "desk": 0xF8F4,
    "brightness": 0xE3AB,
    "brightness_low": 0xE1AD,
    "brightness_high": 0xE1AC,
    "link": 0xE250,
    "link_off": 0xE16F,
    "home": 0xE9B2,
    "power": 0xF8C7,
    "bolt": 0xEA0B,
    "hub": 0xE9F4,
    "sensors": 0xE51E,
    "add": 0xE145,
    "remove": 0xE15B,
    "save": 0xE161,
    "restart": 0xF053,
    "memory": 0xE322,
    "walk": 0xE536,
    "error": 0xF8B6,
    "warning": 0xF083,
    "cloud": 0xF15C,
    "cloud_done": 0xE2BF,
    "cloud_off": 0xE2C1,
    "rotate": 0xF2D5,
    "bedtime": 0xF159,
    "schedule": 0xEFD6,
    "height": 0xEA16,
    "swap_vert": 0xE8D5,
    "gradient": 0xE3E9,
    "auto_awesome": 0xE65F,
    "waves": 0xE176,
    "looks": 0xE3FC,
    "contrast": 0xEB37,
    "water_drop": 0xE798,
    "touch": 0xE913,
    "history": 0xE8B3,
    "speed": 0xE9E4,
    "home_iot": 0xE283,
    "display_settings": 0xEB97,
    "sync": 0xE627,
    "chevron_right": 0xE5CC,
    "pan_tool": 0xE769,  # front_hand
    "block": 0xF08C,
    "developer_board": 0xE30D,
    "hourglass": 0xE88B,  # hourglass_empty
}

# (c symbol, source, axes, features to freeze, size px, glyph ranges, symbols, fallback)
FONTS = [
    ("ui_font_text_14", "Inter.ttf", {"wght": 600, "opsz": 14}, [], 14, [TEXT_RANGE], TEXT_EXTRA, "ui_font_icon_18"),
    ("ui_font_text_16", "Inter.ttf", {"wght": 500, "opsz": 16}, [], 16, [TEXT_RANGE], TEXT_EXTRA, "ui_font_icon_18"),
    ("ui_font_text_20", "Inter.ttf", {"wght": 600, "opsz": 20}, [], 20, [TEXT_RANGE], TEXT_EXTRA, "ui_font_icon_24"),
    ("ui_font_text_28", "Inter.ttf", {"wght": 600, "opsz": 28}, ["tnum"], 28, [TEXT_RANGE], TEXT_EXTRA, "ui_font_icon_32"),
    ("ui_font_display_64", "Inter.ttf", {"wght": 600, "opsz": 32}, ["tnum"], 64, ["0x20-0x5A"], "", None),
    ("ui_font_icon_18", "MaterialSymbolsRounded.ttf", {"FILL": 1, "GRAD": 0, "opsz": 20, "wght": 500}, [], 18, None, None, None),
    ("ui_font_icon_24", "MaterialSymbolsRounded.ttf", {"FILL": 1, "GRAD": 0, "opsz": 24, "wght": 500}, [], 24, None, None, None),
    ("ui_font_icon_32", "MaterialSymbolsRounded.ttf", {"FILL": 1, "GRAD": 0, "opsz": 40, "wght": 500}, [], 32, None, None, None),
    ("ui_font_icon_48", "MaterialSymbolsRounded.ttf", {"FILL": 0, "GRAD": 0, "opsz": 48, "wght": 600}, [], 48, None, None, None),
]


def rel(path: Path) -> str:
    """Paths relative to the repo root keep machine-specific paths out of the generated files."""
    try:
        return str(Path(path).resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def run(cmd: list) -> None:
    args = [rel(c) if isinstance(c, Path) else str(c) for c in cmd]
    print("  $", " ".join(args[:6]), "..." if len(args) > 6 else "")
    subprocess.run(args, check=True, cwd=ROOT)


def ensure_sources() -> None:
    SRC.mkdir(parents=True, exist_ok=True)
    for name, url in DOWNLOADS.items():
        dest = SRC / name
        if not dest.exists():
            print(f"downloading {name}")
            urllib.request.urlretrieve(url, dest)


def instance(name: str, src: str, axes: dict, features: list[str], unicodes: str | None) -> Path:
    BUILD.mkdir(parents=True, exist_ok=True)
    inst = BUILD / f"{name}.inst.ttf"
    source = SRC / src
    if unicodes:
        # Subset first: the icon font is ~15 MB and instancing it whole is slow.
        sub = BUILD / f"{name}.sub.ttf"
        run([str(BIN / "pyftsubset"), source, f"--unicodes={unicodes}", f"--output-file={rel(sub)}",
             "--layout-features=*", "--no-hinting"])
        source = sub
    run([str(BIN / "fonttools"), "varLib.instancer", source, *[f"{k}={v}" for k, v in axes.items()], "-o", inst, "-q"])
    if features:
        frozen = BUILD / f"{name}.frozen.ttf"
        run([str(BIN / "pyftfeatfreeze"), "-f", ",".join(features), inst, frozen])
        return frozen
    return inst


def utf8_escape(cp: int) -> str:
    return "".join(f"\\x{b:02X}" for b in chr(cp).encode("utf-8"))


def generate() -> None:
    ensure_sources()
    OUT.mkdir(parents=True, exist_ok=True)
    icon_ranges = ",".join(f"0x{cp:X}" for cp in sorted(set(ICONS.values())))
    icon_unicodes = ",".join(f"U+{cp:04X}" for cp in sorted(set(ICONS.values())))

    for name, src, axes, feats, size, ranges, symbols, fallback in FONTS:
        print(f"[{name}]")
        is_icon = ranges is None
        ttf = instance(name, src, axes, feats, icon_unicodes if is_icon else None)
        out_c = OUT / f"{name}.c"
        cmd = ["npx", "-y", "lv_font_conv@1.5.3", "--no-compress", "--bpp", "4", "--size", str(size),
               "--format", "lvgl", "--lv-include", "lvgl.h", "--font", ttf, "-o", out_c]
        if is_icon:
            cmd += ["-r", icon_ranges]
        else:
            for r in ranges:
                cmd += ["-r", r]
            if symbols:
                cmd += ["--symbols", symbols]
        if fallback:
            cmd += ["--lv-fallback", fallback]
        run(cmd)

    write_header()
    for lic in ("Inter-OFL.txt", "MaterialSymbols-LICENSE.txt"):
        (OUT / lic).write_bytes((SRC / lic).read_bytes())


def write_header() -> None:
    lines = [
        "// Generated by tools/fonts/generate_fonts.py - do not edit.",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for name, *_ in FONTS:
        lines.append(f"extern const lv_font_t {name};")
    lines += ["", "#ifdef __cplusplus", "}", "#endif", "", "// Material Symbols Rounded glyphs (UTF-8)"]
    width = max(len(n) for n in ICONS)
    for icon, cp in ICONS.items():
        lines.append(f'#define UI_ICON_{icon.upper():<{width}} "{utf8_escape(cp)}"')
    lines.append("")
    (OUT / "ui_fonts.h").write_text("\n".join(lines))
    print(f"wrote {OUT / 'ui_fonts.h'}")


if __name__ == "__main__":
    generate()
