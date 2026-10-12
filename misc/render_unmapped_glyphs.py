# /// script
# dependencies = ["pillow"]
# ///
"""Render a labelled contact sheet for checking the remaining font substitutions."""

import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    root = Path(__file__).resolve().parent.parent
    output = root / "tmp" / "translation_memory"
    report = json.loads((output / "font_mapping_report.json").read_text(encoding="utf-8"))
    glyphs = report["unmatched"]
    font = ImageFont.truetype(str(root / "tmp" / "molebox" / "files" / "Font"), 94)
    label_font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 16)
    columns, width, height = 10, 140, 140
    image = Image.new("RGB", (columns * width, ((len(glyphs) + columns - 1) // columns) * height), "white")
    draw = ImageDraw.Draw(image)
    for i, item in enumerate(glyphs):
        x, y = (i % columns) * width, (i // columns) * height
        draw.rectangle((x, y, x + width - 1, y + height - 1), outline="#cccccc")
        draw.text((x + 6, y + 4), f"{i:02d} U+{ord(item['encoded']):04X}", fill="black", font=label_font)
        draw.line((x + 5, y + 115, x + width - 5, y + 115), fill="#dddddd")
        draw.text((x + width / 2, y + 115), item["encoded"], fill="black", font=font, anchor="ms")
    image.save(output / "unmapped_glyphs.png")
    print("Rendered", len(glyphs), "glyphs.")


if __name__ == "__main__":
    main()
