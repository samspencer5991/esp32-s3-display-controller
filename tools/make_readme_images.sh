#!/bin/sh
# Regenerates docs/images from the desktop simulator.
set -e
cd "$(dirname "$0")/.."
cmake -S sim -B build-sim -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-sim -j8 --target desk_sim >/dev/null
OUT=docs/images
mkdir -p "$OUT"
./build-sim/desk_sim --script sim/scripts/readme.sim --out "$OUT" | grep -v Warn
./build-sim/desk_sim --script sim/scripts/themes.sim --out build-sim/themes | grep -v Warn
tools/.venv/bin/python - <<'PY'
from PIL import Image
imgs = [Image.open(f"build-sim/themes/theme-{i}.png") for i in range(7)]
gap = 8
sheet = Image.new("RGB", (640 * 2 + gap, (180 + gap) * 4 - gap), (40, 44, 52))
for i, im in enumerate(imgs):
    sheet.paste(im, ((i % 2) * (640 + gap), (i // 2) * (180 + gap)))
sheet.save("docs/images/themes.png")
PY
echo "docs/images updated"
