#include "tdl_display_manage.h"
#include "tal_log.h"
#include "tkl_memory.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    ALLOC_NONE = 0,
    ALLOC_SRAM,
    ALLOC_PSRAM,
} alloc_kind_t;

typedef struct {
    void *base;
    void *user;
    size_t size;
    alloc_kind_t kind;
} allocation_t;

#define MAX_ALLOCATIONS 16U

static allocation_t s_allocations[MAX_ALLOCATIONS];
static void *s_last_fill;
static size_t s_last_fill_size;
static unsigned s_fill_calls;
static unsigned s_copy_calls;

OPERATE_RET tal_log_print_secure(BOOL_T is_const_fmt, const TAL_LOG_LEVEL_E level,
                                 const char *file, const int line, const char *fmt, ...)
{
    (void)is_const_fmt;
    (void)level;
    (void)file;
    (void)line;
    (void)fmt;
    return OPRT_OK;
}

static allocation_t *find_allocation(const void *ptr)
{
    for (unsigned i = 0U; i < MAX_ALLOCATIONS; ++i) {
        if (s_allocations[i].kind != ALLOC_NONE && s_allocations[i].user == ptr) {
            return &s_allocations[i];
        }
    }
    return NULL;
}

static void *record_allocation(void *base, void *user, size_t size, alloc_kind_t kind)
{
    for (unsigned i = 0U; i < MAX_ALLOCATIONS; ++i) {
        if (s_allocations[i].kind == ALLOC_NONE) {
            s_allocations[i] = (allocation_t){base, user, size, kind};
            return user;
        }
    }
    assert(0 && "allocation tracker exhausted");
    return NULL;
}

void *tkl_system_malloc(size_t size)
{
    void *ptr = malloc(size);
    return ptr ? record_allocation(ptr, ptr, size, ALLOC_SRAM) : NULL;
}

void tkl_system_free(void *ptr)
{
    if (ptr == NULL) return;
    allocation_t *allocation = find_allocation(ptr);
    assert(allocation != NULL && allocation->kind == ALLOC_SRAM);
    free(allocation->base);
    memset(allocation, 0, sizeof(*allocation));
}

void *tkl_system_psram_malloc(size_t size)
{
    void *base = malloc(size + TKL_SYSTEM_PSRAM_ALIGNMENT - 1U);
    if (base == NULL) return NULL;
    uintptr_t aligned = ((uintptr_t)base + TKL_SYSTEM_PSRAM_ALIGNMENT - 1U) &
                        ~(uintptr_t)(TKL_SYSTEM_PSRAM_ALIGNMENT - 1U);
    return record_allocation(base, (void *)aligned, size, ALLOC_PSRAM);
}

void tkl_system_psram_free(void *ptr)
{
    if (ptr == NULL) return;
    allocation_t *allocation = find_allocation(ptr);
    assert(allocation != NULL && allocation->kind == ALLOC_PSRAM);
    free(allocation->base);
    memset(allocation, 0, sizeof(*allocation));
}

void *tkl_system_memset(void *dst, int value, size_t size)
{
    allocation_t *allocation = find_allocation(dst);
    assert(allocation == NULL || allocation->kind == ALLOC_PSRAM);
    s_last_fill = dst;
    s_last_fill_size = size;
    ++s_fill_calls;
    return memset(dst, value, size);
}

void *tkl_system_memcpy(void *dst, const void *src, size_t size)
{
    allocation_t *dst_allocation = find_allocation(dst);
    allocation_t *src_allocation = find_allocation(src);
    assert((dst_allocation == NULL || dst_allocation->kind == ALLOC_PSRAM) ||
           (src_allocation == NULL || src_allocation->kind == ALLOC_PSRAM));
    ++s_copy_calls;
    return memcpy(dst, src, size);
}

uint8_t tdl_disp_get_fmt_bpp(TUYA_DISPLAY_PIXEL_FMT_E pixel_fmt)
{
    switch (pixel_fmt) {
        case TUYA_PIXEL_FMT_RGB565: return 16U;
        case TUYA_PIXEL_FMT_RGB888: return 24U;
        case TUYA_PIXEL_FMT_MONOCHROME: return 1U;
        case TUYA_PIXEL_FMT_I2: return 2U;
        default: return 0U;
    }
}

static void reset_tracker(void)
{
    memset(s_allocations, 0, sizeof(s_allocations));
    s_last_fill = NULL;
    s_last_fill_size = 0U;
    s_fill_calls = 0U;
    s_copy_calls = 0U;
}

static unsigned active_allocations(void)
{
    unsigned active = 0U;
    for (unsigned i = 0U; i < MAX_ALLOCATIONS; ++i) {
        if (s_allocations[i].kind != ALLOC_NONE) ++active;
    }
    return active;
}

static void psram_framebuffer_uses_split_allocation(void)
{
    reset_tracker();
    const uint32_t frame_len = 4097U;
    TDL_DISP_FRAME_BUFF_T *fb = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, frame_len);
    assert(fb != NULL);
    assert(fb->type == DISP_FB_TP_PSRAM);
    assert(fb->len == frame_len);
    assert(fb->frame != NULL && fb->frame_alloc != NULL);
    assert(((uintptr_t)fb->frame & 63U) == 0U);

    allocation_t *descriptor = find_allocation(fb);
    allocation_t *frame = find_allocation(fb->frame_alloc);
    assert(descriptor != NULL && descriptor->kind == ALLOC_SRAM);
    assert(frame != NULL && frame->kind == ALLOC_PSRAM);
    assert(fb->frame >= (uint8_t *)fb->frame_alloc);
    assert(fb->frame + frame_len <= (uint8_t *)fb->frame_alloc + frame->size);

    assert(s_fill_calls == 1U);
    assert(s_last_fill == fb->frame && s_last_fill_size == frame_len);
    for (uint32_t i = 0U; i < frame_len; ++i) assert(fb->frame[i] == 0U);

    tdl_disp_free_frame_buff(fb);
    assert(active_allocations() == 0U);
}

static void sram_framebuffer_stays_in_sram(void)
{
    reset_tracker();
    TDL_DISP_FRAME_BUFF_T *fb = tdl_disp_create_frame_buff(DISP_FB_TP_SRAM, 511U);
    assert(fb != NULL);
    assert(fb->type == DISP_FB_TP_SRAM);
    assert(((uintptr_t)fb->frame & 63U) == 0U);
    assert(find_allocation(fb) != NULL && find_allocation(fb)->kind == ALLOC_SRAM);
    assert(find_allocation(fb->frame_alloc) != NULL &&
           find_allocation(fb->frame_alloc)->kind == ALLOC_SRAM);
    assert(s_fill_calls == 0U);
    tdl_disp_free_frame_buff(fb);
    assert(active_allocations() == 0U);
}

static void invalid_length_is_rejected_without_leaking(void)
{
    reset_tracker();
    assert(tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, UINT32_MAX) == NULL);
    assert(active_allocations() == 0U);
}

static void psram_draw_fill_and_copy_use_safe_access(void)
{
    reset_tracker();
    TDL_DISP_FRAME_BUFF_T *fb = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, 4U * 2U * 2U);
    assert(fb != NULL);
    fb->fmt = TUYA_PIXEL_FMT_RGB565;
    fb->width = 4U;
    fb->height = 2U;

    unsigned copies_before = s_copy_calls;
    assert(tdl_disp_draw_fill_full(fb, 0x0000F800U, false) == OPRT_OK);
    assert(s_copy_calls > copies_before);
    for (uint32_t i = 0U; i < 8U; ++i) {
        assert(fb->frame[i * 2U] == 0x00U);
        assert(fb->frame[i * 2U + 1U] == 0xF8U);
    }

    uint8_t source[4] = {0x11U, 0x22U, 0x33U, 0x44U};
    TDL_DISP_RECT_T rect = {.x0 = 1U, .y0 = 0U, .x1 = 2U, .y1 = 0U};
    copies_before = s_copy_calls;
    assert(tdl_disp_draw_copy(fb, &rect, source) == OPRT_OK);
    assert(s_copy_calls > copies_before);
    assert(memcmp(&fb->frame[2], source, sizeof(source)) == 0);

    copies_before = s_copy_calls;
    assert(tdl_disp_draw_point(fb, 0U, 1U, 0x000007E0U, false) == OPRT_OK);
    assert(s_copy_calls > copies_before);
    assert(fb->frame[8] == 0xE0U && fb->frame[9] == 0x07U);

    tdl_disp_free_frame_buff(fb);
    assert(active_allocations() == 0U);
}

int main(void)
{
    psram_framebuffer_uses_split_allocation();
    sram_framebuffer_stays_in_sram();
    invalid_length_is_rejected_without_leaking();
    psram_draw_fill_and_copy_use_safe_access();
    puts("display framebuffer memory routing checks passed");
    return 0;
}
