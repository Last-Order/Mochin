"""生成待机页面可由 LVGL 直接使用的动画与场景资源。

动画源图第一排按 192×208 划分为 8 格。前 7 格为动画帧，第 8 格必须
完全透明。动画输出文件顺序为：

    frame 0 RGB565 | frame 0 A8 | ... | frame 6 RGB565 | frame 6 A8

屋内场景源图缩放为屏幕原生 240×240 后输出纯 RGB565。两类资源都使用
ESP32-S3 的小端内存字节序；lcd_display 在最终 SPI flush 时统一执行面板
所需的字节交换。
"""

from pathlib import Path

from PIL import Image


FRAME_WIDTH = 192
FRAME_HEIGHT = 208
GRID_COLUMNS = 8
ANIMATION_FRAME_COUNT = 7
SCENE_SIZE = 240

COMPONENT_DIR = Path(__file__).resolve().parent.parent
ANIMATION_SOURCE_PATH = (
    COMPONENT_DIR / "assets" / "purin_spritesheet.webp"
)
ANIMATION_OUTPUT_PATH = (
    COMPONENT_DIR / "assets" / "purin_idle.rgb565a8"
)
INDOOR_SCENE_SOURCE_PATH = (
    COMPONENT_DIR / "assets" / "scene_indoor.png"
)
INDOOR_SCENE_OUTPUT_PATH = (
    COMPONENT_DIR / "assets" / "scene_indoor.rgb565"
)


def encode_rgb565(image: Image.Image) -> bytes:
    """把不透明图片转换为 LVGL 原生 RGB565 小端字节布局。"""
    rgb = image.convert("RGB")
    encoded = bytearray()
    pixels = rgb.tobytes()

    for offset in range(0, len(pixels), 3):
        red, green, blue = pixels[offset : offset + 3]
        pixel = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        encoded.extend((pixel & 0xFF, pixel >> 8))

    return bytes(encoded)


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


def generate_animation_asset() -> None:
    """校验雪碧图第一排并写出 7 帧动画资源。"""
    sprite = Image.open(ANIMATION_SOURCE_PATH).convert("RGBA")
    expected_width = FRAME_WIDTH * GRID_COLUMNS
    if sprite.width != expected_width or sprite.height < FRAME_HEIGHT:
        raise ValueError(
            f"unexpected spritesheet size {sprite.size}; "
            f"expected width {expected_width} and at least "
            f"{FRAME_HEIGHT} px high"
        )

    frames: list[Image.Image] = []
    for column in range(GRID_COLUMNS):
        left = column * FRAME_WIDTH
        frame = sprite.crop(
            (left, 0, left + FRAME_WIDTH, FRAME_HEIGHT)
        )
        alpha_bounds = frame.getchannel("A").getbbox()

        if column < ANIMATION_FRAME_COUNT:
            if alpha_bounds is None:
                raise ValueError(f"animation frame {column} is unexpectedly empty")
            frames.append(frame)
        elif alpha_bounds is not None:
            raise ValueError("the eighth cell in the first row must be empty")

    encoded = b"".join(encode_rgb565a8(frame) for frame in frames)
    expected_size = (
        ANIMATION_FRAME_COUNT * FRAME_WIDTH * FRAME_HEIGHT * 3
    )
    if len(encoded) != expected_size:
        raise AssertionError(
            f"encoded size {len(encoded)} does not match {expected_size}"
        )

    ANIMATION_OUTPUT_PATH.write_bytes(encoded)
    print(
        f"generated {ANIMATION_OUTPUT_PATH} "
        f"({ANIMATION_FRAME_COUNT} frames, {len(encoded)} bytes)"
    )


def generate_indoor_scene_asset() -> None:
    """将正方形屋内插画缩放并写出全屏 RGB565 背景。"""
    scene = Image.open(INDOOR_SCENE_SOURCE_PATH)
    if scene.width != scene.height:
        raise ValueError(
            f"indoor scene must be square, got {scene.size}"
        )

    scene = scene.convert("RGB").resize(
        (SCENE_SIZE, SCENE_SIZE),
        Image.Resampling.LANCZOS,
    )
    encoded = encode_rgb565(scene)
    expected_size = SCENE_SIZE * SCENE_SIZE * 2
    if len(encoded) != expected_size:
        raise AssertionError(
            f"encoded size {len(encoded)} does not match {expected_size}"
        )

    INDOOR_SCENE_OUTPUT_PATH.write_bytes(encoded)
    print(
        f"generated {INDOOR_SCENE_OUTPUT_PATH} "
        f"({SCENE_SIZE}x{SCENE_SIZE}, {len(encoded)} bytes)"
    )


def main() -> None:
    generate_animation_asset()
    generate_indoor_scene_asset()


if __name__ == "__main__":
    main()
