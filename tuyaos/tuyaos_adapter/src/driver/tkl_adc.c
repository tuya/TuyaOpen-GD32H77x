/**
 * @file tkl_adc.c
 * @brief GD32H77X ADC TKL backend.
 *
 * Default external-channel routing lives in this driver.  tkl_pinmux only
 * updates a controller/channel entry before tkl_adc_init(); init applies the
 * resulting table directly in analog mode.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_adc.h"

#ifdef BIT
#undef BIT
#endif

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_adc.h"
#include "tkl_pinmux.h"
#include "tkl_system.h"
#include "tuya_error_code.h"

#include <string.h>

#define GD32H77X_ADC_CONTROLLERS      3U
#define GD32H77X_ADC_CHANNELS         22U
#define GD32H77X_ADC_TIMEOUT_MS       50U
#define GD32H77X_ADC_DEFAULT_REF_MV   3300U

typedef struct {
    uint32_t port;
    uint32_t pin;
    uint8_t valid;
} ADC_PIN_T;

typedef struct {
    uint8_t initialized;
    uint8_t width;
    uint8_t ch_nums;
    uint16_t conv_cnt;
    uint32_t ch_list;
    TUYA_ADC_MODE_E mode;
    uint32_t ref_vol;
} GD32H77X_ADC_STATE;

#define ADC_PIN(port_name, pin_num) \
    {port_name, GPIO_PIN_##pin_num, 1U}

static const uint32_t s_adc_periph[GD32H77X_ADC_CONTROLLERS] = {
    ADC0, ADC1, ADC2,
};
static const uint32_t s_adc_rcu[GD32H77X_ADC_CONTROLLERS] = {
    RCU_ADC0, RCU_ADC1, RCU_ADC2,
};
static const adc_idx_enum s_adc_rcu_idx[GD32H77X_ADC_CONTROLLERS] = {
    IDX_ADC0, IDX_ADC1, IDX_ADC2,
};
static GD32H77X_ADC_STATE s_adc[TUYA_ADC_NUM_MAX];

/* External analog-channel defaults transcribed from GD32H779xx DS Rev.0.5.
 * Each entry includes the matching TUYA_GPIO_NUM so a pinmux override never
 * needs a second chip-to-Tuya pin lookup.
 */
static ADC_PIN_T s_adc_pins[GD32H77X_ADC_CONTROLLERS][GD32H77X_ADC_CHANNELS] = {
    [TUYA_ADC_NUM_0] = {
        [2]  = ADC_PIN(GPIOF, 11), /* PF11 / TUYA_GPIO_NUM_91  / ADC0_IN2 */
        [3]  = ADC_PIN(GPIOA, 6),  /* PA6  / TUYA_GPIO_NUM_6   / ADC0_IN3 */
        [4]  = ADC_PIN(GPIOC, 4),  /* PC4  / TUYA_GPIO_NUM_36  / ADC0_IN4 */
        [5]  = ADC_PIN(GPIOB, 1),  /* PB1  / TUYA_GPIO_NUM_17  / ADC0_IN5 */
        [6]  = ADC_PIN(GPIOF, 12), /* PF12 / TUYA_GPIO_NUM_92  / ADC0_IN6 */
        [7]  = ADC_PIN(GPIOA, 7),  /* PA7  / TUYA_GPIO_NUM_7   / ADC0_IN7 */
        [8]  = ADC_PIN(GPIOC, 5),  /* PC5  / TUYA_GPIO_NUM_37  / ADC0_IN8 */
        [9]  = ADC_PIN(GPIOB, 0),  /* PB0  / TUYA_GPIO_NUM_16  / ADC0_IN9 */
        [10] = ADC_PIN(GPIOC, 0),  /* PC0  / TUYA_GPIO_NUM_32  / ADC0_IN10 */
        [11] = ADC_PIN(GPIOC, 1),  /* PC1  / TUYA_GPIO_NUM_33  / ADC0_IN11 */
        [14] = ADC_PIN(GPIOA, 2),  /* PA2  / TUYA_GPIO_NUM_2   / ADC0_IN14 */
        [15] = ADC_PIN(GPIOA, 3),  /* PA3  / TUYA_GPIO_NUM_3   / ADC0_IN15 */
        [16] = ADC_PIN(GPIOA, 0),  /* PA0  / TUYA_GPIO_NUM_0   / ADC0_IN16 */
        [17] = ADC_PIN(GPIOA, 1),  /* PA1  / TUYA_GPIO_NUM_1   / ADC0_IN17 */
        [18] = ADC_PIN(GPIOA, 4),  /* PA4  / TUYA_GPIO_NUM_4   / ADC0_IN18 */
        [19] = ADC_PIN(GPIOA, 5),  /* PA5  / TUYA_GPIO_NUM_5   / ADC0_IN19 */
    },
    [TUYA_ADC_NUM_1] = {
        [2]  = ADC_PIN(GPIOF, 13), /* PF13 / TUYA_GPIO_NUM_93  / ADC1_IN2 */
        [3]  = ADC_PIN(GPIOA, 6),  /* PA6  / TUYA_GPIO_NUM_6   / ADC1_IN3 */
        [4]  = ADC_PIN(GPIOC, 4),  /* PC4  / TUYA_GPIO_NUM_36  / ADC1_IN4 */
        [5]  = ADC_PIN(GPIOB, 1),  /* PB1  / TUYA_GPIO_NUM_17  / ADC1_IN5 */
        [6]  = ADC_PIN(GPIOF, 14), /* PF14 / TUYA_GPIO_NUM_94  / ADC1_IN6 */
        [7]  = ADC_PIN(GPIOA, 7),  /* PA7  / TUYA_GPIO_NUM_7   / ADC1_IN7 */
        [8]  = ADC_PIN(GPIOC, 5),  /* PC5  / TUYA_GPIO_NUM_37  / ADC1_IN8 */
        [9]  = ADC_PIN(GPIOB, 0),  /* PB0  / TUYA_GPIO_NUM_16  / ADC1_IN9 */
        [10] = ADC_PIN(GPIOC, 0),  /* PC0  / TUYA_GPIO_NUM_32  / ADC1_IN10 */
        [11] = ADC_PIN(GPIOC, 1),  /* PC1  / TUYA_GPIO_NUM_33  / ADC1_IN11 */
        [13] = ADC_PIN(GPIOC, 3),  /* PC3  / TUYA_GPIO_NUM_35  / ADC1_IN13 */
        [14] = ADC_PIN(GPIOA, 2),  /* PA2  / TUYA_GPIO_NUM_2   / ADC1_IN14 */
        [15] = ADC_PIN(GPIOA, 3),  /* PA3  / TUYA_GPIO_NUM_3   / ADC1_IN15 */
        [18] = ADC_PIN(GPIOA, 4),  /* PA4  / TUYA_GPIO_NUM_4   / ADC1_IN18 */
        [19] = ADC_PIN(GPIOA, 5),  /* PA5  / TUYA_GPIO_NUM_5   / ADC1_IN19 */
    },
    [TUYA_ADC_NUM_2] = {
        [0]  = ADC_PIN(GPIOC, 2),  /* PC2  / TUYA_GPIO_NUM_34  / ADC2_IN0 */
        [1]  = ADC_PIN(GPIOC, 3),  /* PC3  / TUYA_GPIO_NUM_35  / ADC2_IN1 */
        [2]  = ADC_PIN(GPIOF, 9),  /* PF9  / TUYA_GPIO_NUM_89  / ADC2_IN2 */
        [3]  = ADC_PIN(GPIOF, 7),  /* PF7  / TUYA_GPIO_NUM_87  / ADC2_IN3 */
        [4]  = ADC_PIN(GPIOF, 5),  /* PF5  / TUYA_GPIO_NUM_85  / ADC2_IN4 */
        [5]  = ADC_PIN(GPIOF, 3),  /* PF3  / TUYA_GPIO_NUM_83  / ADC2_IN5 */
        [6]  = ADC_PIN(GPIOF, 10), /* PF10 / TUYA_GPIO_NUM_90  / ADC2_IN6 */
        [7]  = ADC_PIN(GPIOF, 8),  /* PF8  / TUYA_GPIO_NUM_88  / ADC2_IN7 */
        [8]  = ADC_PIN(GPIOF, 6),  /* PF6  / TUYA_GPIO_NUM_86  / ADC2_IN8 */
        [9]  = ADC_PIN(GPIOF, 4),  /* PF4  / TUYA_GPIO_NUM_84  / ADC2_IN9 */
        [10] = ADC_PIN(GPIOC, 0),  /* PC0  / TUYA_GPIO_NUM_32  / ADC2_IN10 */
        [11] = ADC_PIN(GPIOC, 1),  /* PC1  / TUYA_GPIO_NUM_33  / ADC2_IN11 */
        [13] = ADC_PIN(GPIOH, 2),  /* PH2  / TUYA_GPIO_NUM_114 / ADC2_IN13 */
        [14] = ADC_PIN(GPIOH, 3),  /* PH3  / TUYA_GPIO_NUM_115 / ADC2_IN14 */
        [15] = ADC_PIN(GPIOH, 4),  /* PH4  / TUYA_GPIO_NUM_116 / ADC2_IN15 */
        [16] = ADC_PIN(GPIOH, 5),  /* PH5  / TUYA_GPIO_NUM_117 / ADC2_IN16 */
    },
};

void __tkl_adc_set_pin(TUYA_ADC_NUM_E port, uint8_t channel,
                       uint32_t gpio_port, uint32_t gpio_pin)
{
    if (port >= GD32H77X_ADC_CONTROLLERS ||
        channel >= GD32H77X_ADC_CHANNELS) {
        return;
    }
    s_adc_pins[port][channel].port = gpio_port;
    s_adc_pins[port][channel].pin = gpio_pin;
    s_adc_pins[port][channel].valid = 1U;
}

static uint32_t __adc_gpio_rcu(uint32_t port)
{
    static const uint32_t ports[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOJ, GPIOK,
    };
    static const uint32_t rcus[] = {
        RCU_GPIOA, RCU_GPIOB, RCU_GPIOC, RCU_GPIOD, RCU_GPIOE,
        RCU_GPIOF, RCU_GPIOG, RCU_GPIOH, RCU_GPIOJ, RCU_GPIOK,
    };

    for (uint32_t i = 0U; i < sizeof(ports) / sizeof(ports[0]); ++i) {
        if (ports[i] == port) {
            return rcus[i];
        }
    }
    return 0U;
}

static void __adc_config_gpio(TUYA_ADC_NUM_E port, uint8_t channel)
{
    const ADC_PIN_T *pin = &s_adc_pins[port][channel];
    uint32_t rcu = __adc_gpio_rcu(pin->port);

    if (pin->valid == 0U || rcu == 0U) {
        return;
    }
    rcu_periph_clock_enable(rcu);
    gpio_mode_set(pin->port, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, pin->pin);
}

static uint32_t __adc_resolution(uint8_t width)
{
    switch (width) {
        case 8U:  return ADC_RESOLUTION_8B;
        case 10U: return ADC_RESOLUTION_10B;
        case 12U: return ADC_RESOLUTION_12B;
        case 14U: return ADC_RESOLUTION_14B;
        default:  return UINT32_MAX;
    }
}

static uint8_t __adc_popcount(uint32_t value)
{
    uint8_t count = 0U;
    while (value != 0U) {
        value &= value - 1U;
        ++count;
    }
    return count;
}

static OPERATE_RET __adc_wait_eorc(uint32_t periph, uint32_t timeout_ms)
{
    uint32_t start_ms = (uint32_t)tkl_system_get_millisecond();

    while (adc_flag_get(periph, ADC_FLAG_EORC) == RESET) {
        if ((uint32_t)tkl_system_get_millisecond() - start_ms >= timeout_ms) {
            return OPRT_TIMEOUT;
        }
    }
    return OPRT_OK;
}

static OPERATE_RET __adc_convert_once(TUYA_ADC_NUM_E port, uint8_t channel,
                                      uint32_t *raw)
{
    uint32_t periph;
    uint32_t timeout_ms;
    OPERATE_RET rt;

    if (raw == NULL || port >= GD32H77X_ADC_CONTROLLERS ||
        channel >= GD32H77X_ADC_CHANNELS || s_adc[port].initialized == 0U ||
        s_adc_pins[port][channel].valid == 0U) {
        return OPRT_INVALID_PARM;
    }

    periph = s_adc_periph[port];
    adc_special_function_config(periph, ADC_CONTINUOUS_MODE | ADC_SCAN_MODE,
                                DISABLE);
    adc_external_trigger_config(periph, ADC_ROUTINE_CHANNEL,
                                EXTERNAL_TRIGGER_DISABLE);
    adc_channel_length_config(periph, ADC_ROUTINE_CHANNEL, 1U);
    adc_routine_channel_config(periph, 0U, channel, 0x1FFU);
    adc_flag_clear(periph, ADC_FLAG_EORC);
    adc_software_trigger_enable(periph, ADC_ROUTINE_CHANNEL);

    timeout_ms = GD32H77X_ADC_TIMEOUT_MS +
                 (uint32_t)s_adc[port].conv_cnt;
    rt = __adc_wait_eorc(periph, timeout_ms);
    if (rt == OPRT_OK) {
        *raw = adc_routine_data_read(periph);
        adc_flag_clear(periph, ADC_FLAG_EORC);
    }
    return rt;
}

OPERATE_RET tkl_adc_init(TUYA_ADC_NUM_E port, TUYA_ADC_BASE_CFG_T *cfg)
{
    uint32_t periph;
    uint32_t resolution;

    if (port >= TUYA_ADC_NUM_MAX || cfg == NULL ||
        cfg->ch_nums == 0U || cfg->ch_nums > 16U ||
        cfg->conv_cnt == 0U ||
        __adc_popcount(cfg->ch_list.data) != cfg->ch_nums) {
        return OPRT_INVALID_PARM;
    }
    if (port >= GD32H77X_ADC_CONTROLLERS) {
        return OPRT_NOT_SUPPORTED;
    }

    resolution = __adc_resolution(cfg->width);
    if (resolution == UINT32_MAX ||
        (port == TUYA_ADC_NUM_2 && cfg->width == 14U)) {
        return OPRT_NOT_SUPPORTED;
    }

    for (uint8_t channel = 0U; channel < GD32H77X_ADC_CHANNELS; ++channel) {
        if ((cfg->ch_list.data & (1UL << channel)) != 0U) {
            if (s_adc_pins[port][channel].valid == 0U) {
                return OPRT_NOT_SUPPORTED;
            }
            __adc_config_gpio(port, channel);
        }
    }

    periph = s_adc_periph[port];
    rcu_adc_clock_config(s_adc_rcu_idx[port], RCU_ADCSRC_PER);
    rcu_periph_clock_enable(s_adc_rcu[port]);
    adc_deinit(periph);
    adc_clock_config(periph, ADC_CLK_ASYNC_DIV16);
    adc_data_alignment_config(periph, ADC_DATAALIGN_RIGHT);
    adc_resolution_config(periph, resolution);
    adc_special_function_config(periph, ADC_SCAN_MODE,
                                cfg->mode == TUYA_ADC_SCAN ? ENABLE : DISABLE);
    adc_special_function_config(periph, ADC_CONTINUOUS_MODE,
                                cfg->mode == TUYA_ADC_CONTINUOUS ? ENABLE : DISABLE);
    adc_enable(periph);
    tkl_system_sleep(2U);
    adc_calibration_number(periph, ADC_CALIBRATION_NUM7);
    adc_calibration_enable(periph);

    memset(&s_adc[port], 0, sizeof(s_adc[port]));
    s_adc[port].initialized = 1U;
    s_adc[port].width = cfg->width;
    s_adc[port].ch_nums = cfg->ch_nums;
    s_adc[port].conv_cnt = cfg->conv_cnt;
    s_adc[port].ch_list = cfg->ch_list.data;
    s_adc[port].mode = cfg->mode;
    s_adc[port].ref_vol = cfg->ref_vol == 0U ? GD32H77X_ADC_DEFAULT_REF_MV : cfg->ref_vol;
    return OPRT_OK;
}

OPERATE_RET tkl_adc_deinit(TUYA_ADC_NUM_E port)
{
    if (port >= TUYA_ADC_NUM_MAX) {
        return OPRT_INVALID_PARM;
    }
    if (port >= GD32H77X_ADC_CONTROLLERS || s_adc[port].initialized == 0U) {
        return OPRT_OK;
    }

    adc_disable(s_adc_periph[port]);
    memset(&s_adc[port], 0, sizeof(s_adc[port]));
    return OPRT_OK;
}

uint8_t tkl_adc_width_get(TUYA_ADC_NUM_E port)
{
    return port < TUYA_ADC_NUM_MAX ? s_adc[port].width : 0U;
}

uint32_t tkl_adc_ref_voltage_get(TUYA_ADC_NUM_E port)
{
    if (port >= TUYA_ADC_NUM_MAX || s_adc[port].initialized == 0U) {
        return GD32H77X_ADC_DEFAULT_REF_MV;
    }
    return s_adc[port].ref_vol;
}

int32_t tkl_adc_temperature_get(void)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_adc_read_single_channel(TUYA_ADC_NUM_E port, uint8_t ch_id,
                                        int32_t *data)
{
    uint32_t raw;

    if (data == NULL) {
        return OPRT_INVALID_PARM;
    }
    OPERATE_RET rt = __adc_convert_once(port, ch_id, &raw);
    if (rt == OPRT_OK) {
        *data = (int32_t)raw;
    }
    return rt;
}

OPERATE_RET tkl_adc_read_data(TUYA_ADC_NUM_E port, int32_t *buff, uint16_t len)
{
    uint32_t out_index = 0U;

    if (port >= TUYA_ADC_NUM_MAX || buff == NULL ||
        s_adc[port].initialized == 0U) {
        return OPRT_INVALID_PARM;
    }
    if ((uint32_t)s_adc[port].ch_nums * s_adc[port].conv_cnt > len) {
        return OPRT_INVALID_PARM;
    }

    for (uint8_t channel = 0U; channel < 16U; ++channel) {
        if ((s_adc[port].ch_list & (1UL << channel)) == 0U) {
            continue;
        }
        for (uint16_t sample = 0U; sample < s_adc[port].conv_cnt; ++sample) {
            uint32_t raw;
            OPERATE_RET rt = __adc_convert_once(port, channel, &raw);
            if (rt != OPRT_OK) {
                return rt;
            }
            buff[out_index++] = (int32_t)raw;
        }
    }

    return OPRT_OK;
}

OPERATE_RET tkl_adc_read_voltage(TUYA_ADC_NUM_E port, int32_t *buff, uint16_t len)
{
    if (port >= TUYA_ADC_NUM_MAX || buff == NULL) {
        return OPRT_INVALID_PARM;
    }
    OPERATE_RET rt = tkl_adc_read_data(port, buff, len);
    if (rt != OPRT_OK) {
        return rt;
    }

    uint32_t full_scale = (1UL << s_adc[port].width) - 1U;
    for (uint16_t i = 0U; i < len; ++i) {
        buff[i] = (int32_t)(((uint64_t)(uint32_t)buff[i] * s_adc[port].ref_vol +
                             full_scale / 2U) / full_scale);
    }
    return OPRT_OK;
}

OPERATE_RET tkl_adc_ioctl(ADC_IOCTL_CMD_E cmd, void *args)
{
    (void)cmd;
    (void)args;
    return OPRT_NOT_SUPPORTED;
}
