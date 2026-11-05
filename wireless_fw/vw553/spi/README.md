# GD32VW553 SPI firmware

当前尚未发布经过实板验收的 SPI gateway 固件。

后续 SPI 版本应直接放在本目录，命名包含版本但不增加版本子目录，例如：

```text
vw553_at_gateway_spi_v0.1.0.bin
manifest.json
SHA256SUMS
```

SPI 固件必须独立完成引脚、时序、帧边界、错误恢复和与 H779 的联合验收。
不得把 `../uart/` 下的 UART 镜像用于 SPI 接线。
