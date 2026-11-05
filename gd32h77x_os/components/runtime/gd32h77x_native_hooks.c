/**
 * @file gd32h77x_native_hooks.c
 * @brief FreeRTOS hooks shared by standalone GD32H77X projects.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

/*
 * Standalone projects use the empty fallback. Integration layers such as the
 * TuyaOpen adapter provide a strong tick hook and override it at link time.
 */
__attribute__((weak)) void vApplicationTickHook(void)
{
}
