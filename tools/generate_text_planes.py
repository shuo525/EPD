"""Render UTF-8 text into UC8179 black/white and red bit planes.

The generated files are raw binary frames. They are not sent to the display
by this script. The frame format is 48,000 bytes K/W followed by 48,000 bytes
RED, with bit 7 representing the leftmost pixel in each byte.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

WIDTH = 800
HEIGHT = 480
ROW_BYTES = WIDTH // 8
FRAME_BYTES = ROW_BYTES * HEIGHT


def find_font(font_arg: str | None) -> Path:
    if font_arg:
        return Path(font_arg)
    candidates = (
        Path(r"C:\Windows\Fonts\simhei.ttf"),
        Path(r"C:\Windows\Fonts\msyh.ttc"),
        Path(r"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("No Chinese font found; pass --font FONT_FILE")


def render(text: str, font_path: Path, size: int) -> Image.Image:
    image = Image.new("RGB", (WIDTH, HEIGHT), "white")
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(str(font_path), size)
    box = draw.textbbox((0, 0), text, font=font)
    x = (WIDTH - (box[2] - box[0])) // 2 - box[0]
    y = (HEIGHT - (box[3] - box[1])) // 2 - box[1]
    draw.text((x, y), text, font=font, fill="black")
    return image


def planes(image: Image.Image) -> tuple[bytes, bytes]:
    kw = bytearray(FRAME_BYTES)
    red = bytearray([0xFF]) * FRAME_BYTES
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b = image.getpixel((x, y))
            # This script renders black text on white. Keep the classifier
            # explicit so red drawing can be added without changing format.
            is_red = r >= 150 and r > g * 1.3 and r > b * 1.3
            is_black = r < 100 and g < 100 and b < 100
            if not (is_red or is_black):
                continue
            index = y * ROW_BYTES + x // 8
            mask = 1 << (7 - (x % 8))
            kw[index] |= mask
            if is_red:
                red[index] &= ~mask
    return bytes(kw), bytes(red)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--text", default="擎翌智能李博士")
    parser.add_argument("--font", help="TTF/TTC font path")
    parser.add_argument("--size", type=int, default=64)
    parser.add_argument(
        "--output-dir", type=Path, default=Path("assets/generated/text")
    )
    args = parser.parse_args()

    font_path = find_font(args.font)
    image = render(args.text, font_path, args.size)
    kw, red = planes(image)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "text_kw.bin").write_bytes(kw)
    (args.output_dir / "text_red.bin").write_bytes(red)
    (args.output_dir / "text_frame.bin").write_bytes(kw + red)
    with (args.output_dir / "text_frame.hex").open("w", encoding="ascii") as output:
        frame = kw + red
        for offset in range(0, len(frame), 16):
            output.write(" ".join(f"{value:02X}" for value in frame[offset : offset + 16]))
            output.write("\n")
    image.save(args.output_dir / "text_preview.png")
    print(f"Text: {args.text}")
    print(f"Font: {font_path}")
    print(f"K/W plane: {len(kw)} bytes")
    print(f"RED plane: {len(red)} bytes")
    print(f"Combined frame: {len(kw) + len(red)} bytes")
    print(f"Output: {args.output_dir.resolve()}")


if __name__ == "__main__":
    main()
