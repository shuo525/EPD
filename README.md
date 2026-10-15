# UC8179 三色墨水屏

基于 nRF52832 DK / PCA10040 和 nRF Connect SDK 的 UC8179 800×480 三色屏 BLE 显示工程。

## 目录

```text
.
├── assets/
│   ├── input/                 # 输入图片
│   └── generated/             # 脚本输出的平面数据和预览
├── docs/                      # 屏幕手册和 BLE 协议
├── dts/bindings/              # UC8179 设备树 binding
├── src/                       # 固件源码和当前烧录使用的图像资源
├── tools/                     # 图片、文字点阵生成脚本
├── app.overlay                # 屏幕 SPI/GPIO 引脚
├── prj.conf                   # Zephyr 配置
└── CMakeLists.txt             # 构建配置
```

## 生成图片点阵

需要安装 Pillow。默认读取指定输入图片，并将平面数据、预览图和 C 资源写入 `assets/generated/image/`：

```powershell
python tools/convert_epd_image.py assets/input/3.png
```

脚本生成的 `image_asset.c/.h` 是资源文件。若要将新图片作为固件启动时的默认画面，将它们放入 `src/` 替换当前资源后重新构建；BLE 上传图片则直接使用生成的 `image_kw.bin` 和 `image_red.bin`。

## 生成文字点阵

```powershell
python tools/generate_text_planes.py --text "擎翌智能李博士"
```

默认输出到 `assets/generated/text/`。生成两个各 48,000 字节的二进制平面和预览图。

## BLE 图片传输

按顺序发送 `FRAME:KW` 和黑白平面、`FRAME:RED` 和红色平面，最后发送 `FRAME:SHOW`。每个命令阶段都应等待 Status 特征返回的 READY/DONE 状态。完整协议和 UUID 见 [docs/BLE_FRAME_PROTOCOL.md](docs/BLE_FRAME_PROTOCOL.md)。

## 工程构建

工程使用 Zephyr / nRF Connect SDK 构建。目标板为 `nrf52dk/nrf52832`；引脚配置见 `app.overlay`。用户可在 nRF Connect for VS Code 中选择对应工具链、目标板并构建。
