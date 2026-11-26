#include "tkl_memory.h"
#include "tuya_ringbuf.h"
#include <string.h>

#define RINGBUFF_FREE   tkl_system_free
#define RINGBUFF_MALLOC tkl_system_malloc

#define GET_MIN(x, y) ((x) < (y) ? (x) : (y))
#define GET_MAX(x, y) ((x) > (y) ? (x) : (y))

/*
 * ringbuff structure
 */
typedef struct {
    RINGBUFF_TYPE_E type; ///< ringbuff type
    uint32_t in;          ///< position of input
    uint32_t out;         ///< position of output
    uint32_t len;         ///< length of buff data
    bool psram;           ///< payload is in mapped PSRAM
    uint8_t *buff;        ///< ring buff
} __RINGBUFF_T;

#define RINGBUFF_PSRAM_ALIGNMENT 4U

static void __ringbuff_init(__RINGBUFF_T *ringbuff, uint32_t len)
{
    ringbuff->in = 0;
    ringbuff->out = 0;
    ringbuff->len = len;
}

#if TUYA_RINGBUF_PSRAM_ENABLED
static bool __ringbuff_copy_to_psram(__RINGBUFF_T *ringbuff, uint8_t *dst,
                                     const void *src, uint32_t len)
{
    if (ringbuff->psram) {
        return tkl_system_memcpy(dst, src, len) != NULL;
    }
    memcpy(dst, src, len);
    return true;
}

static bool __ringbuff_copy_from_psram(__RINGBUFF_T *ringbuff, void *dst,
                                       const uint8_t *src, uint32_t len)
{
    if (ringbuff->psram) {
        return tkl_system_memcpy(dst, src, len) != NULL;
    }
    memcpy(dst, src, len);
    return true;
}
#endif

OPERATE_RET tuya_ring_buff_create(uint32_t len, RINGBUFF_TYPE_E type, TUYA_RINGBUFF_T *ringbuff)
{
    __RINGBUFF_T *rbuff = NULL;
    __RINGBUFF_T **out_ring_buff = (__RINGBUFF_T **)ringbuff;
#if TUYA_RINGBUF_PSRAM_ENABLED
    bool psram = type == OVERFLOW_PSRAM_STOP_TYPE;
#else
    bool psram = false;
#endif

    if (type == OVERFLOW_COVERAGE_TYPE ||
        type == OVERFLOW_PSRAM_COVERAGE_TYPE) {
        return OPRT_NOT_SUPPORTED;
    }

    if (ringbuff == NULL || len == 0 ||
        (type != OVERFLOW_STOP_TYPE && type != OVERFLOW_PSRAM_STOP_TYPE)) {
        return OPRT_INVALID_PARM;
    }

#if TUYA_RINGBUF_PSRAM_ENABLED
    if (psram) {
        rbuff = (__RINGBUFF_T *)RINGBUFF_MALLOC(sizeof(*rbuff));
        if (rbuff == NULL) {
            return OPRT_MALLOC_FAILED;
        }
        rbuff->buff = tkl_system_psram_malloc(len);
        if (rbuff->buff == NULL ||
            ((uintptr_t)rbuff->buff & (RINGBUFF_PSRAM_ALIGNMENT - 1U)) != 0U) {
            if (rbuff->buff != NULL) {
                tkl_system_psram_free(rbuff->buff);
            }
            RINGBUFF_FREE(rbuff);
            return OPRT_MALLOC_FAILED;
        }
    } else
#endif
    {
        rbuff = (__RINGBUFF_T *)RINGBUFF_MALLOC(sizeof(*rbuff) + len);
        if (rbuff == NULL) {
            return OPRT_MALLOC_FAILED;
        }
        rbuff->buff = (uint8_t *)(rbuff + 1);
    }
    rbuff->type = type;
    rbuff->psram = psram;
    __ringbuff_init(rbuff, len);
    *out_ring_buff = rbuff;

    return OPRT_OK;
}

OPERATE_RET tuya_ring_buff_free(TUYA_RINGBUFF_T ringbuff)
{
    __RINGBUFF_T *rbuff = (__RINGBUFF_T *)ringbuff;

    if (rbuff == NULL) {
        return OPRT_INVALID_PARM;
    }
#if TUYA_RINGBUF_PSRAM_ENABLED
    if (rbuff->psram) {
        tkl_system_psram_free(rbuff->buff);
    }
#endif
    RINGBUFF_FREE(rbuff);

    return OPRT_OK;
}

OPERATE_RET tuya_ring_buff_reset(TUYA_RINGBUFF_T ringbuff)
{
    __RINGBUFF_T *rbuff = (__RINGBUFF_T *)ringbuff;

    if (rbuff == NULL) {
        return OPRT_INVALID_PARM;
    }
    __ringbuff_init(rbuff, rbuff->len);

    return OPRT_OK;
}

uint32_t tuya_ring_buff_free_size_get(TUYA_RINGBUFF_T ringbuff)
{
    uint32_t size, in, out;
    __RINGBUFF_T *rbuff = (__RINGBUFF_T *)ringbuff;

    if (rbuff == NULL) {
        return 0;
    }

    in = rbuff->in;
    out = rbuff->out;
    if (in == out) {
        size = rbuff->len;
    } else if (out > in) {
        size = out - in;
    } else {
        size = rbuff->len - (in - out);
    }

    return size - 1;
}

uint32_t tuya_ring_buff_used_size_get(TUYA_RINGBUFF_T ringbuff)
{
    uint32_t size, in, out;
    __RINGBUFF_T *rbuff = (__RINGBUFF_T *)ringbuff;

    if (rbuff == NULL) {
        return 0;
    }

    in = rbuff->in;
    out = rbuff->out;
    if (in == out) {
        size = 0;
    } else if (in > out) {
        size = in - out;
    } else {
        size = rbuff->len - (out - in);
    }

    return size;
}

uint32_t tuya_ring_buff_write(TUYA_RINGBUFF_T ringbuff, const void *data, uint32_t len)
{
    uint32_t tmp_len;
    uint32_t free_len;
    const uint8_t *pdata = data;
    __RINGBUFF_T *rbuff = (__RINGBUFF_T *)ringbuff;

    if (rbuff == NULL || data == NULL || len == 0) {
        return 0;
    }
    // overwriting unread parts is not supported when the write is full
    free_len = tuya_ring_buff_free_size_get(rbuff);
    len = GET_MIN(free_len, len);
    if (len == 0) {
        return 0;
    }

#if TUYA_RINGBUF_PSRAM_ENABLED
    uint32_t old_in = rbuff->in;
#endif

    // write data to remaining buff
    tmp_len = GET_MIN(rbuff->len - rbuff->in, len);
#if TUYA_RINGBUF_PSRAM_ENABLED
    if (!__ringbuff_copy_to_psram(rbuff, &rbuff->buff[rbuff->in],
                                  pdata, tmp_len)) {
        return 0;
    }
#else
    memcpy(&rbuff->buff[rbuff->in], pdata, tmp_len);
#endif
    rbuff->in += tmp_len;
    len -= tmp_len;

    // write remaining data to beginning of buffer
    if (len > 0) {
#if TUYA_RINGBUF_PSRAM_ENABLED
        if (!__ringbuff_copy_to_psram(rbuff, rbuff->buff,
                                      &pdata[tmp_len], len)) {
            rbuff->in = old_in;
            return 0;
        }
#else
        memcpy(rbuff->buff, &pdata[tmp_len], len);
#endif
        rbuff->in = len;
    }

    // buff loopback check
    if (rbuff->in >= rbuff->len) {
        rbuff->in = 0;
    }

    return tmp_len + len;
}

uint32_t tuya_ring_buff_read(TUYA_RINGBUFF_T ringbuff, void *data, uint32_t len)
{
    uint32_t tmp_len;
    uint32_t used_len;
    uint8_t *pdata = data;
    __RINGBUFF_T *rbuff = (__RINGBUFF_T *)ringbuff;

    if (rbuff == NULL || data == NULL || len == 0) {
        return 0;
    }

    used_len = tuya_ring_buff_used_size_get(rbuff);
    len = GET_MIN(used_len, len);
    if (len == 0) {
        return 0;
    }

#if TUYA_RINGBUF_PSRAM_ENABLED
    uint32_t old_out = rbuff->out;
#endif

    // read data from linear part of buffer
    tmp_len = GET_MIN(rbuff->len - rbuff->out, len);
#if TUYA_RINGBUF_PSRAM_ENABLED
    if (!__ringbuff_copy_from_psram(rbuff, pdata,
                                    &rbuff->buff[rbuff->out], tmp_len)) {
        return 0;
    }
#else
    memcpy(pdata, &rbuff->buff[rbuff->out], tmp_len);
#endif
    rbuff->out += tmp_len;
    len -= tmp_len;

    // read data from beginning of buffer (overflow part)
    if (len > 0) {
#if TUYA_RINGBUF_PSRAM_ENABLED
        if (!__ringbuff_copy_from_psram(rbuff, &pdata[tmp_len],
                                        rbuff->buff, len)) {
            rbuff->out = old_out;
            return 0;
        }
#else
        memcpy(&pdata[tmp_len], rbuff->buff, len);
#endif
        rbuff->out = len;
    }

    // check end of buffer
    if (rbuff->out >= rbuff->len) {
        rbuff->out = 0;
    }

    return tmp_len + len;
}

uint32_t tuya_ring_buff_discard(TUYA_RINGBUFF_T ringbuff, uint32_t len)
{
    uint32_t tmp_len;
    uint32_t used_len;
    __RINGBUFF_T *rbuff = (__RINGBUFF_T *)ringbuff;

    if(rbuff == NULL || len == 0) {
        return 0;
    }

    used_len = tuya_ring_buff_used_size_get(rbuff);
    len = GET_MIN(used_len, len);
    if (len == 0) {
        return 0;
    }

    // discard data from linear part of buffer
    tmp_len = GET_MIN(rbuff->len - rbuff->out, len);
    rbuff->out += tmp_len;
    len -= tmp_len;

    // discard data from beginning of buffer (overflow part)
    if (len > 0) {
        rbuff->out = len;
    }

    // check end of buffer
    if (rbuff->out >= rbuff->len) {
        rbuff->out = 0;
    }

    return tmp_len + len;
}

uint32_t tuya_ring_buff_peek(TUYA_RINGBUFF_T ringbuff, void *data, uint32_t len)
{
    uint32_t out;
    uint32_t tmp_len;
    uint32_t used_len;
    uint8_t *pdata = data;
    __RINGBUFF_T *rbuff = (__RINGBUFF_T *)ringbuff;

    if (rbuff == NULL || data == NULL || len == 0) {
        return 0;
    }

    out = rbuff->out;
    used_len = tuya_ring_buff_used_size_get(rbuff);

    len = GET_MIN(len, used_len);
    if (len == 0) {
        return 0;
    }

    tmp_len = GET_MIN(rbuff->len - out, len);
#if TUYA_RINGBUF_PSRAM_ENABLED
    if (!__ringbuff_copy_from_psram(rbuff, pdata,
                                    &rbuff->buff[out], tmp_len)) {
        return 0;
    }
#else
    memcpy(pdata, &rbuff->buff[out], tmp_len);
#endif
    len -= tmp_len;

    if (len > 0) {
#if TUYA_RINGBUF_PSRAM_ENABLED
        if (!__ringbuff_copy_from_psram(rbuff, &pdata[tmp_len],
                                        rbuff->buff, len)) {
            return 0;
        }
#else
        memcpy(&pdata[tmp_len], rbuff->buff, len);
#endif
    }

    return tmp_len + len;
}
