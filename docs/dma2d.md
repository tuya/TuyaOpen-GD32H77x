# GD32H77X DMA2D (IPA) 适配

GD32H77X 没有 STM32 风格的 DMA2D，硬件对位模块是 IPA（Image Processing Accelerator）。
TKL 使用公共 `tkl_dma2d` 接口，底层由 `tuyaos_adapter/src/driver/tkl_dma2d.c` 映射到 IPA。

## 能力

| 源格式 | 目标格式 | 操作 |
|---|---|---|
| RGB565 | RGB565 | 硬件二维块拷贝 |
| RGB888 | RGB888 | 硬件二维块拷贝 |
| RGB565 | RGB888 | 格式转换 |
| RGB888 | RGB565 | 格式转换 |
| UYVY422 | RGB565 | YCbCr 格式转换 |
| UYVY422 | RGB888 | YCbCr 格式转换 |

IPA 没有 YUV422 目标格式，因此以 `TUYA_FRAME_FMT_YUV422` 为目标的
`convert` / `memcpy` 返回 `OPRT_NOT_SUPPORTED`，不会回退到 CPU 拷贝。

`TUYA_FRAME_FMT_YUV422` 在 H77X 上按公共相机默认的 packed UYVY 解释，并使用
YCbCr（128 为中心）转换参数。

## 裁剪与 stride

`TKL_DMA2D_FRAME_INFO_T.axis` 指定源/目标块在各自 frame 中的起始像素。
`width_cp` / `height_cp` 为 0 时复制整帧，否则复制指定块，并自动裁剪到源、目标边界。

IPA 的 `FLOFF/BLOFF/DLOFF` 单位是像素，表示当前行复制结束后额外跳过的像素数，
因此驱动配置为：

```text
line_offset = frame.width - copy_width
buffer_addr = frame.pbuf + (axis.y * frame.width + axis.x) * bytes_per_pixel
```

这不是整行 stride，也不是字节偏移。

## 异步完成

`tkl_dma2d_convert()` 和 `tkl_dma2d_memcpy()` 配置并启动 IPA 后立即返回。
`IPA_IRQHandler()` 处理以下事件：

- `IPA_INT_FTF`：正常完成，回调 `TUYA_DMA2D_TRANS_COMPLETE_ISR`
- `IPA_INT_TAE` / `IPA_INT_WCF`：访问错误或配置错误，回调
  `TUYA_DMA2D_TRANS_ERROR_ISR`

上层 `tal_dma2d_wait_finish()` 依赖该回调释放传输上下文。

## 配置与验证

```text
CONFIG_ENABLE_DMA2D=y
```

实机自测例程：

```text
examples/peripherals/dma2d/selftest
```

覆盖带 stride/坐标的 RGB565 块拷贝、RGB565↔RGB888 往返转换和
UYVY422→RGB565 转换。
