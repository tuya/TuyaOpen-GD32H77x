# GD32H77X mapped PSRAM TKL 使用说明

本文适用于启用以下配置的板级应用：

```text
CONFIG_ENABLE_GD32H77X_PSRAM=y
CONFIG_GD32H77X_PSRAM_ACCESS_MAPPED=y
CONFIG_ENABLE_PSRAM=y
```

PSRAM 初始化和最终进入映射模式由平台 `app_entry.c` 统一完成。应用不要调用
`tkl_system_psram_init()`，也不要在板级注册函数中重复初始化。

## 对外接口

```c
void *tkl_system_psram_malloc(size_t size);
void tkl_system_psram_free(void *ptr);
void *tkl_system_psram_memcpy(void *dst, const void *src, size_t size);
void *tkl_system_psram_memset(void *dst, int value, size_t size);
int tkl_system_psram_get_free_heap_size(void);
```

这是有意保持很小的一组接口。业务需要大块 PSRAM 时，优先使用
`memcpy`/`memset` 搬运和填充数据，不要把 PSRAM 指针当作普通 SRAM 指针使用。

## 正确用法

```c
size_t len = 128 * 1024U;
uint8_t *psram = tkl_system_psram_malloc(len);
if (psram == NULL) {
    /* 分配失败 */
    return;
}

/* SRAM -> PSRAM，允许任意字节长度和未对齐偏移 */
if (tkl_system_psram_memcpy(psram, source, len) == NULL) {
    tkl_system_psram_free(psram);
    return;
}

/* 填充 PSRAM，允许任意字节长度 */
tkl_system_psram_memset(psram + 17U, 0xA5, 1000U);

/* PSRAM -> SRAM */
tkl_system_psram_memcpy(destination, psram, len);

tkl_system_psram_free(psram);
```

`tkl_system_psram_memcpy()` 支持：

- SRAM → PSRAM
- PSRAM → SRAM
- PSRAM → PSRAM
- 任意字节长度和任意字节偏移

内部会将访问收敛到 4 字节对齐 mapped word，并在每个 word 写入后执行
`DSB`、等待 OSPI BUSY 清零。该实现优先保证稳定性，带宽低于普通 SRAM。

## 禁止用法

不要对 PSRAM 指针直接调用标准库函数：

```c
memcpy(psram + 1, source, 17);  /* 错误 */
memset(psram + 3, 0, 100);      /* 错误 */
strcpy(psram, "text");          /* 错误 */
```

也不要直接进行 byte/half-word mapped 写：

```c
psram[1] = value;               /* 错误 */
*(uint16_t *)(psram + 2) = val; /* 错误 */
```

确需直接访问映射区时，只允许经过验证的 32 位对齐 word 访问。

## 其他约束

- 分配地址按 32 字节对齐，对应 `TKL_SYSTEM_PSRAM_ALIGNMENT`。
- `tkl_system_psram_malloc()` 得到的指针必须使用 `tkl_system_psram_free()` 释放。
- PSRAM 映射堆末尾 32 字节为保护区，不要访问。
- 这些接口不是 ISR 安全接口；并发访问同一块 PSRAM 时由调用者加锁。
- `memcpy`/`memset` 当前是低速高可靠实现，不适合高吞吐连续流；需要更高带宽时
  应先完成波形、时序和压力测试后再做优化。
- 未经验证不要把 PSRAM 缓冲用于 DMA、Cache Write-Back 或需要普通 C 内存语义的场景。
- 普通小内存仍应使用内部 SRAM；PSRAM 适合图片、音频、网络和大块临时缓冲。

## 验证工程

`examples/system/psram_tkl_minimal` 会验证：

- allocator 多阶段、多位置、多轮 32 位读写；
- `tkl_system_psram_memcpy()` 的 SRAM/PSRAM 双向搬运；
- `tkl_system_psram_memset()` 的未对齐偏移和任意长度填充；
- PSRAM ring 的随机 write/read/peek/discard 和回卷；
- 所有块释放后 PSRAM 空闲堆完全恢复。
