"""从 Purin 雪碧图第一排生成供 LVGL 直接使用的 RGB565+A8 资源。

源图第一排按 192×192 划分为 8 格，前 7 格为动画帧，第 8 格必须完全透明。
输出文件顺序为：

    frame 0 RGB565 | frame 0 A8 | ... | frame 6 RGB565 | frame 6 A8

RGB565 使用 ESP32-S3 的小端内存字节序；lcd_display 在最终 SPI flush 时
统一执行面板所需的字节交换。
"""

from pathlib import Path

from PIL import Image


FRAME_SIZE = 192
GRID_COLUMNS = 8
ANIMATION_FRAME_COUNT = 7

COMPONENT_DIR = Path(__file__).resolve().parent.parent
SOURCE_PATH = COMPONENT_DIR / "assets" / "purin_spritesheet.webp"
OUTPUT_PATH = COMPONENT_DIR / "assets" / "purin_idle.rgb565a8"


def encode_rgb565a8(frame: Image.Image) -> bytes:
    """把一帧 RGBA 转换为 LVGL 的颜色平面加透明度平面布局。"""
    rgba = frame.convert("RGBA")
    color_plane = bytearray()
    alpha_plane = bytearray()
    pixels = rgba.tobytes()

    for offset in range(0, len(pixels), 4):
        red, green, blue, alpha = pixels[offset : offset + 4]
        pixel = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        color_plane.extend((pixel & 0xFF, pixel >> 8))
        alpha_plane.append(alpha)

    return bytes(color_plane + alpha_plane)


def main() -> None:
    sprite = Image.open(SOURCE_PATH).convert("RGBA")
    expected_width = FRAME_SIZE * GRID_COLUMNS
    if sprite.width != expected_width or sprite.height < FRAME_SIZE:
        raise ValueError(
            f"unexpected spritesheet size {sprite.size}; "
            f"expected width {expected_width} and at least {FRAME_SIZE} px high"
        )

    frames: list[Image.Image] = []
    for column in range(GRID_COLUMNS):
        left = column * FRAME_SIZE
        frame = sprite.crop((left, 0, left + FRAME_SIZE, FRAME_SIZE))
        alpha_bounds = frame.getchannel("A").getbbox()

        if column < ANIMATION_FRAME_COUNT:
            if alpha_bounds is None:
                raise ValueError(f"animation frame {column} is unexpectedly empty")
            frames.append(frame)
        elif alpha_bounds is not None:
            raise ValueError("the eighth cell in the first row must be empty")

    encoded = b"".join(encode_rgb565a8(frame) for frame in frames)
    expected_size = (
        ANIMATION_FRAME_COUNT * FRAME_SIZE * FRAME_SIZE * 3
    )
    if len(encoded) != expected_size:
        raise AssertionError(
            f"encoded size {len(encoded)} does not match {expected_size}"
        )

    OUTPUT_PATH.write_bytes(encoded)
    print(
        f"generated {OUTPUT_PATH} "
        f"({ANIMATION_FRAME_COUNT} frames, {len(encoded)} bytes)"
    )


if __name__ == "__main__":
    main()
