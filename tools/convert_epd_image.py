"""Convert an image to UC8179 800x480 K/W and red planes."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageOps


WIDTH = 800
HEIGHT = 480
ROW_BYTES = WIDTH // 8
FRAME_BYTES = ROW_BYTES * HEIGHT


def c_array(name: str, data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("\t" + ", ".join(f"0x{value:02X}" for value in chunk) + ",")
    return f"const uint8_t {name}[EPD_IMAGE_FRAME_BYTES] = {{\n" + "\n".join(lines) + "\n};\n"


def convert(source: Path) -> tuple[bytes, bytes, Image.Image]:
    with Image.open(source) as opened:
        rgb = ImageOps.fit(
            opened.convert("RGB"),
            (WIDTH, HEIGHT),
            method=Image.Resampling.LANCZOS,
            centering=(0.5, 0.5),
        )

    # Detect red from the resized source colors.  Applying RGB autocontrast
    # first stretches each channel independently and can turn a large, dark
    # red area into near-black before it reaches the red classifier.
    pixels = list(rgb.getdata())

    red_mask = []
    gray_values = []
    for red, green, blue in pixels:
        dominant = red - max(green, blue)
        is_red = red >= 70 and dominant >= 15 and red * 100 >= green * 112
        red_mask.append(is_red)
        if is_red:
            gray_values.append(255)
        else:
            gray_values.append((red * 299 + green * 587 + blue * 114) // 1000)

    gray = Image.new("L", (WIDTH, HEIGHT))
    gray.putdata(gray_values)
    dithered = gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    mono = list(dithered.getdata())

    kw_plane = bytearray(FRAME_BYTES)
    red_plane = bytearray([0xFF]) * FRAME_BYTES
    preview = Image.new("RGB", (WIDTH, HEIGHT), "white")
    preview_pixels = preview.load()

    for index, is_red in enumerate(red_mask):
        x = index % WIDTH
        y = index // WIDTH
        byte_index = y * ROW_BYTES + x // 8
        bit = 1 << (7 - (x % 8))

        if is_red:
            kw_plane[byte_index] |= bit
            red_plane[byte_index] &= ~bit
            preview_pixels[x, y] = (220, 0, 0)
        elif mono[index] == 0:
            kw_plane[byte_index] |= bit
            preview_pixels[x, y] = (0, 0, 0)

    return bytes(kw_plane), bytes(red_plane), preview


def make_solid_red() -> tuple[bytes, bytes, Image.Image]:
    """Create the UC8179 plane combination used for a full red screen."""
    kw_plane = bytes([0xFF]) * FRAME_BYTES
    red_plane = bytes([0x00]) * FRAME_BYTES
    preview = Image.new("RGB", (WIDTH, HEIGHT), (220, 0, 0))
    return kw_plane, red_plane, preview


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("--output-dir", type=Path, default=Path("src"))
    parser.add_argument(
        "--solid-red",
        action="store_true",
        help="generate a full-screen red diagnostic pattern",
    )
    args = parser.parse_args()

    if args.solid_red:
        kw_plane, red_plane, preview = make_solid_red()
    else:
        if args.source is None:
            parser.error("source is required unless --solid-red is used")
        kw_plane, red_plane, preview = convert(args.source)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    header = """#ifndef IMAGE_ASSET_H_
#define IMAGE_ASSET_H_

#include <stdint.h>

#define EPD_IMAGE_WIDTH 800
#define EPD_IMAGE_HEIGHT 480
#define EPD_IMAGE_FRAME_BYTES 48000

extern const uint8_t epd_image_kw[EPD_IMAGE_FRAME_BYTES];
extern const uint8_t epd_image_red[EPD_IMAGE_FRAME_BYTES];

#endif
"""
    source = (
        '#include "image_asset.h"\n\n'
        + c_array("epd_image_kw", kw_plane)
        + "\n"
        + c_array("epd_image_red", red_plane)
    )

    (args.output_dir / "image_asset.h").write_text(header, encoding="ascii")
    (args.output_dir / "image_asset.c").write_text(source, encoding="ascii")
    preview.save(args.output_dir / "image_preview.png")

    print(f"Generated {len(kw_plane)}-byte K/W plane and {len(red_plane)}-byte red plane")
    print(f"Preview: {args.output_dir / 'image_preview.png'}")


if __name__ == "__main__":
    main()
