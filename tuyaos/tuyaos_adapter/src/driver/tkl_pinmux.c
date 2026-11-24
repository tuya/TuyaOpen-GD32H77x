/**
 * @file tkl_pinmux.c
 * @brief GD32H77X pin routing for the GPIO IDs registered by tkl_gpio.
 *
 * Peripheral routes are intentionally explicit. An unsupported pin/function
 * pair is rejected instead of being assigned a plausible but incorrect AF.
 */

#include "tkl_pinmux.h"

#ifdef BIT
#undef BIT
#endif
#include "gd32h77x_78x.h"
#include "gd32h77x_78x_gpio.h"
#include "tuya_error_code.h"

typedef struct {
    uint16_t pin;
    uint16_t func;
    uint8_t af;
    uint8_t mode;
    uint8_t channel;
} PIN_ROUTE_T;

/* Strong setters provided by the always-built TKL driver translation units. */
extern void __tkl_spi_set_pin(TUYA_SPI_NUM_E port, TUYA_PIN_FUNC_E func,
                              uint32_t gpio_port, uint32_t gpio_pin,
                              uint32_t af);
extern void __tkl_qspi_set_pin(TUYA_QSPI_NUM_E port, TUYA_PIN_FUNC_E func,
                               uint32_t gpio_port, uint32_t gpio_pin,
                               uint32_t af);
extern void __tkl_i2c_set_pin(TUYA_I2C_NUM_E port, TUYA_PIN_FUNC_E func,
                              uint32_t gpio_port, uint32_t gpio_pin,
                              uint32_t af);
extern void __tkl_uart_set_pin(TUYA_UART_NUM_E port, TUYA_PIN_FUNC_E func,
                               uint32_t gpio_port, uint32_t gpio_pin,
                               uint32_t af);
extern void __tkl_dvp_set_pin(TUYA_PIN_FUNC_E func, uint32_t gpio_port,
                              uint32_t gpio_pin, uint32_t af);
extern void __tkl_adc_set_pin(TUYA_ADC_NUM_E port, uint8_t channel,
                              uint32_t gpio_port, uint32_t gpio_pin);
extern void __tkl_pwm_set_pin(TUYA_PWM_NUM_E port, TUYA_PIN_FUNC_E func,
                              uint32_t gpio_port, uint32_t gpio_pin,
                              uint32_t af);
extern void __tkl_sdio_set_pin(TUYA_PIN_FUNC_E func, uint32_t gpio_port,
                              uint32_t gpio_pin, uint32_t af);

#define MODE_AF 0U
#define MODE_OD 1U
#define MODE_ANALOG 2U
static const uint32_t s_afs[] = {
    GPIO_AF_0, GPIO_AF_1, GPIO_AF_2, GPIO_AF_3,
    GPIO_AF_4, GPIO_AF_5, GPIO_AF_6, GPIO_AF_7,
    GPIO_AF_8, GPIO_AF_9, GPIO_AF_10, GPIO_AF_11,
    GPIO_AF_12, GPIO_AF_13, GPIO_AF_14, GPIO_AF_15,
    GPIO_AF_16, GPIO_AF_17, GPIO_AF_18,
};
#define R(pin_, func_, af_) {(pin_), (func_), (af_), MODE_AF, 0xFFU}
#define OD(pin_, func_, af_) {(pin_), (func_), (af_), MODE_OD, 0xFFU}
#define A(pin_, func_, channel_) {(pin_), (func_), 0U, MODE_ANALOG, (channel_)}

/* Physical pins and AF values below are transcribed from GD32H779xx DS Rev.0.5.
 * IDs follow the contiguous A-H/J/K mapping defined by TUYA_GPIO_NUM_E. */
static const PIN_ROUTE_T s_routes[] = {
    /* Alternate-function routes verified against the GD32H779xx Rev.0.5 pin tables. */
    R(4, TUYA_SPI0_CS, 5), R(5, TUYA_SPI0_CLK, 5), /* PA4/PA5 */
    R(6, TUYA_SPI0_MISO, 5), R(7, TUYA_SPI0_MOSI, 5), /* PA6/PA7 */
    R(82, TUYA_SPI0_CS, 10), /* PF2 */
    R(6, TUYA_SPI0_MISO, 5), R(6, TUYA_SPI5_MISO, 8), /* PA6 */
    R(0, TUYA_SPI5_CS, 5), /* PA0 */
    R(28, TUYA_SPI1_CS, 5), R(29, TUYA_SPI1_CLK, 5), /* PB12/PB13 */
    R(30, TUYA_SPI1_MISO, 5), R(31, TUYA_SPI1_MOSI, 5), /* PB14/PB15 */
    R(25, TUYA_SPI1_CS, 5), /* PB9 */
    R(75, TUYA_SPI3_CS, 5), R(76, TUYA_SPI3_CLK, 5),
    R(77, TUYA_SPI3_MISO, 5), R(78, TUYA_SPI3_MOSI, 5),
    R(66, TUYA_SPI3_CLK, 5), R(67, TUYA_SPI3_CLK, 5),
    R(68, TUYA_SPI3_CS, 5), R(69, TUYA_SPI3_MISO, 5), R(70, TUYA_SPI3_MOSI, 5),
    R(80, TUYA_SPI3_MISO, 1), R(81, TUYA_SPI3_MOSI, 1),
    R(64, TUYA_SPI4_CS, 1), R(65, TUYA_SPI4_CLK, 1), R(67, TUYA_SPI4_MISO, 2),
    R(71, TUYA_SPI4_MOSI, 5), R(86, TUYA_SPI4_CS, 5), R(87, TUYA_SPI4_CLK, 5),
    R(88, TUYA_SPI4_MISO, 5), R(89, TUYA_SPI4_MOSI, 5),
    R(91, TUYA_SPI4_MOSI, 5), R(118, TUYA_SPI4_CLK, 5),
    R(119, TUYA_SPI4_MISO, 5), R(117, TUYA_SPI4_CS, 5),

    /* SPI5 default group aliases the SPI0 pins at AF8. */
    R(4, TUYA_SPI5_CS, 8), R(5, TUYA_SPI5_CLK, 8),
    R(6, TUYA_SPI5_MISO, 8), R(7, TUYA_SPI5_MOSI, 8),

    /* SPI2 default group. */
    R(15, TUYA_SPI2_CS, 5), /* PA15 */
    R(42, TUYA_SPI2_CLK, 5), R(43, TUYA_SPI2_MISO, 5), /* PC10/PC11 */
    R(44, TUYA_SPI2_MOSI, 5), /* PC12 */

    /* QSPI0 uses SPI3; IO2/IO3 are only exposed in quad mode. */
    R(76, TUYA_QSPI0_SCK, 5), R(66, TUYA_QSPI0_SCK, 5), R(67, TUYA_QSPI0_SCK, 5),
    R(78, TUYA_QSPI0_IO0, 5), R(70, TUYA_QSPI0_IO0, 5), R(81, TUYA_QSPI0_IO0, 1),
    R(77, TUYA_QSPI0_IO1, 5), R(69, TUYA_QSPI0_IO1, 5), R(80, TUYA_QSPI0_IO1, 1),
    R(73, TUYA_QSPI0_IO2, 5), R(74, TUYA_QSPI0_IO3, 5),
    R(62, TUYA_QSPI0_IO2, 5), R(63, TUYA_QSPI0_IO3, 5),
    R(75, TUYA_QSPI0_CS, 5),

    /* I2C routes from GD32H779xx DS Rev.0.5.  Open-drain mode is selected
     * by the pinmux layer; the driver only consumes the updated table. */
    OD(24, TUYA_IIC0_SCL, 4), OD(25, TUYA_IIC0_SDA, 4), /* PB8/PB9 */
    OD(81, TUYA_IIC1_SCL, 4), OD(80, TUYA_IIC1_SDA, 4), /* PF1/PF0 */
    OD(8, TUYA_IIC2_SCL, 4), OD(45, TUYA_IIC2_SDA, 4), /* PA8/PC9 */
    OD(119, TUYA_IIC2_SCL, 4), OD(120, TUYA_IIC2_SDA, 4), /* PH7/PH8 */
    OD(22, TUYA_IIC3_SCL, 6), OD(23, TUYA_IIC3_SDA, 6), /* PB6/PB7 */
    OD(24, TUYA_IIC3_SCL, 6), OD(25, TUYA_IIC3_SDA, 6), /* PB8/PB9 */
    OD(60, TUYA_IIC3_SCL, 4), OD(61, TUYA_IIC3_SDA, 4), /* PD12/PD13 */
    OD(116, TUYA_IIC1_SCL, 4), OD(117, TUYA_IIC1_SDA, 4), /* PH4/PH5 */
    OD(123, TUYA_IIC3_SCL, 4), OD(124, TUYA_IIC3_SDA, 4), /* PH11/PH12 */
    OD(94, TUYA_IIC3_SCL, 4), OD(95, TUYA_IIC3_SDA, 4), /* PF14/PF15 */

    /* UART logical ports map one-to-one to USART0/1/2, UART3/4, USART5. */
    R(9, TUYA_UART0_TX, 7), R(10, TUYA_UART0_RX, 7), /* PA9/PA10 */
    R(22, TUYA_UART0_TX, 7), R(23, TUYA_UART0_RX, 7), /* PB6/PB7 */
    R(84, TUYA_UART0_TX, 4), R(85, TUYA_UART0_RX, 4), /* PF4/PF5 */

    R(2, TUYA_UART1_TX, 7), R(3, TUYA_UART1_RX, 7), /* PA2/PA3 */
    R(53, TUYA_UART1_TX, 7), R(54, TUYA_UART1_RX, 7), /* PD5/PD6 */

    R(114, TUYA_UART2_TX, 1), R(116, TUYA_UART2_RX, 1), /* PH2/PH4 */
    R(56, TUYA_UART2_TX, 7), R(57, TUYA_UART2_RX, 7), /* PD8/PD9 */
    R(26, TUYA_UART2_TX, 7), R(27, TUYA_UART2_RX, 7), /* PB10/PB11 */

    R(0, TUYA_UART3_TX, 8), R(1, TUYA_UART3_RX, 8), /* PA0/PA1 */
    R(48, TUYA_UART3_RX, 8), R(49, TUYA_UART3_TX, 8), /* PD0/PD1 */
    R(42, TUYA_UART3_TX, 8), R(43, TUYA_UART3_RX, 8), /* PC10/PC11 */

    R(29, TUYA_UART4_TX, 14), R(28, TUYA_UART4_RX, 14), /* PB13/PB12 */

    R(38, TUYA_UART5_TX, 7), R(39, TUYA_UART5_RX, 7), /* PC6/PC7 */

    R(72, TUYA_UART6_TX, 7), R(71, TUYA_UART6_RX, 7), /* PE8/PE7 */

    R(65, TUYA_UART7_TX, 8), R(64, TUYA_UART7_RX, 8), /* PE1/PE0 */

    /* PWM default groups: TIMER0/1 on AF1, TIMER2 on AF2. */
    R(8, TUYA_PWM0, 1), R(9, TUYA_PWM1, 1),
    R(10, TUYA_PWM2, 1), R(11, TUYA_PWM3, 1),
    R(0, TUYA_PWM4, 1), R(1, TUYA_PWM5, 1),
    R(2, TUYA_PWM6, 1), R(3, TUYA_PWM7, 1),
    R(6, TUYA_PWM8, 2), R(7, TUYA_PWM9, 2),
    R(16, TUYA_PWM10, 2), R(17, TUYA_PWM11, 2),

    /* DCI/DVP default signal group. */
    R(38, TUYA_DVP0_D0, 13), R(39, TUYA_DVP0_D1, 13),
    R(29, TUYA_DVP0_D2, 13), R(124, TUYA_DVP0_D3, 13),
    R(126, TUYA_DVP0_D4, 13), R(51, TUYA_DVP0_D5, 13),
    R(24, TUYA_DVP0_D6, 13), R(25, TUYA_DVP0_D7, 13),
    R(6, TUYA_DVP0_PIXCLK, 13), R(127, TUYA_DVP0_HSYNC, 1),
    R(23, TUYA_DVP0_VSYNC, 13),

    /* SDIO0 routes. */
    R(44, TUYA_SDIO_CLK, 12),   /* PC12 */
    R(50, TUYA_SDIO_CMD, 12),   /* PD2 */
    R(29, TUYA_SDIO_DATA0, 12), /* PB13, official EVAL group */
    R(40, TUYA_SDIO_DATA0, 12), /* PC8, DESK BUDDY group */
    R(41, TUYA_SDIO_DATA1, 12), /* PC9 */
    R(56, TUYA_SDIO_DATA2, 6),  /* PD8 */
    R(42, TUYA_SDIO_DATA2, 12), /* PC10 */
    R(57, TUYA_SDIO_DATA3, 6),  /* PD9 */
    R(43, TUYA_SDIO_DATA3, 12), /* PC11 */

    /* SDIO1 routes. */
    R(33, TUYA_SDIO_CLK, 9),    /* PC1 */
    R(0,  TUYA_SDIO_CMD, 9),    /* PA0 */
    R(30, TUYA_SDIO_DATA0, 9),  /* PB14 */
    R(31, TUYA_SDIO_DATA1, 9),  /* PB15 */
    R(29, TUYA_SDIO_DATA2, 16), /* PB13 */
    R(20, TUYA_SDIO_DATA3, 9),  /* PB4 */
    R(84, TUYA_SDIO_DATA0, 10), /* PF4 */
    R(85, TUYA_SDIO_DATA1, 10), /* PF5 */
    R(107, TUYA_SDIO_DATA2, 10),/* PG11 */
    R(108, TUYA_SDIO_DATA3, 10),/* PG12 */
    R(105, TUYA_SDIO_DATA0, 11),/* PG9 */
    R(106, TUYA_SDIO_DATA1, 11),/* PG10 */

    /* ADC external channel routes.  Analog mode has no AF value; the last
     * field records the controller channel used by the ADC driver table. */
    A(91, TUYA_ADC0, 2), A(6, TUYA_ADC0, 3), A(36, TUYA_ADC0, 4),
    A(17, TUYA_ADC0, 5), A(92, TUYA_ADC0, 6), A(7, TUYA_ADC0, 7),
    A(37, TUYA_ADC0, 8), A(16, TUYA_ADC0, 9), A(32, TUYA_ADC0, 10),
    A(33, TUYA_ADC0, 11), A(2, TUYA_ADC0, 14), A(3, TUYA_ADC0, 15),
    A(0, TUYA_ADC0, 16), A(1, TUYA_ADC0, 17), A(4, TUYA_ADC0, 18),
    A(5, TUYA_ADC0, 19),
    A(93, TUYA_ADC1, 2), A(6, TUYA_ADC1, 3), A(36, TUYA_ADC1, 4),
    A(17, TUYA_ADC1, 5), A(94, TUYA_ADC1, 6), A(7, TUYA_ADC1, 7),
    A(37, TUYA_ADC1, 8), A(16, TUYA_ADC1, 9), A(32, TUYA_ADC1, 10),
    A(33, TUYA_ADC1, 11), A(35, TUYA_ADC1, 13), A(2, TUYA_ADC1, 14),
    A(3, TUYA_ADC1, 15), A(4, TUYA_ADC1, 18), A(5, TUYA_ADC1, 19),
    A(34, TUYA_ADC2, 0), A(35, TUYA_ADC2, 1), A(89, TUYA_ADC2, 2),
    A(87, TUYA_ADC2, 3), A(85, TUYA_ADC2, 4), A(83, TUYA_ADC2, 5),
    A(90, TUYA_ADC2, 6), A(88, TUYA_ADC2, 7), A(86, TUYA_ADC2, 8),
    A(84, TUYA_ADC2, 9), A(32, TUYA_ADC2, 10), A(33, TUYA_ADC2, 11),
    A(114, TUYA_ADC2, 13), A(115, TUYA_ADC2, 14), A(116, TUYA_ADC2, 15),
    A(117, TUYA_ADC2, 16),
};

#undef R
#undef OD
#undef A

static uint16_t s_last_func[TUYA_GPIO_NUM_MAX];
static uint8_t s_last_valid[TUYA_GPIO_NUM_MAX];

static bool __route_find(uint32_t pin, TUYA_PIN_FUNC_E func,
                         const PIN_ROUTE_T **route)
{
    if (pin >= TUYA_GPIO_NUM_MAX || func == TUYA_PIN_FUNC_MAX) {
        return false;
    }
    for (uint32_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        if (s_routes[i].pin == pin && s_routes[i].func == func) {
            if (route != NULL) {
                *route = &s_routes[i];
            }
            return true;
        }
    }
    return false;
}

static bool __decode(uint32_t pin, uint32_t *port, uint32_t *pad,
                     uint32_t *rcu)
{
    static const uint32_t ports[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOJ, GPIOK,
    };
    static const uint32_t rcus[] = {
        RCU_GPIOA, RCU_GPIOB, RCU_GPIOC, RCU_GPIOD, RCU_GPIOE,
        RCU_GPIOF, RCU_GPIOG, RCU_GPIOH, RCU_GPIOJ, RCU_GPIOK,
    };

    if (port == NULL || pad == NULL || rcu == NULL ||
        pin >= TUYA_GPIO_NUM_MAX) {
        return false;
    }
    uint32_t port_index = pin / 16U;
    uint32_t pin_index = pin % 16U;
    if (port_index >= sizeof(ports) / sizeof(ports[0])) {
        return false;
    }
    *port = ports[port_index];
    *pad = (uint32_t)1U << pin_index;
    *rcu = rcus[port_index];
    return true;
}

static void __update_driver_pin(TUYA_PIN_FUNC_E func, uint32_t gpio_port,
                                 uint32_t gpio_pin, uint32_t af,
                                 uint8_t channel)
{
    if (func <= TUYA_IIC5_SDA) {
        __tkl_i2c_set_pin((TUYA_I2C_NUM_E)(func / 2U), func,
                          gpio_port, gpio_pin, af);
        return;
    }
    if (func >= TUYA_UART0_TX && func <= TUYA_UART7_CTS) {
        uint32_t controller = (func - TUYA_UART0_TX) / 4U;
        __tkl_uart_set_pin((TUYA_UART_NUM_E)controller, func,
                           gpio_port, gpio_pin, af);
        return;
    }
    if ((func >= TUYA_SPI0_MISO && func <= TUYA_SPI2_CS) ||
        (func >= TUYA_SPI3_MISO && func <= TUYA_SPI5_CS)) {
        uint32_t controller = func <= TUYA_SPI2_CS ?
            (func - TUYA_SPI0_MISO) / 4U :
            (func - TUYA_SPI3_MISO) / 4U + 3U;
        __tkl_spi_set_pin((TUYA_SPI_NUM_E)controller, func,
                          gpio_port, gpio_pin, af);
        return;
    }
    if (func >= TUYA_QSPI0_SCK && func <= TUYA_QSPI0_CS) {
        uint32_t controller = (func - TUYA_QSPI0_SCK) / 6U;
        __tkl_qspi_set_pin((TUYA_QSPI_NUM_E)controller, func,
                           gpio_port, gpio_pin, af);
        return;
    }
    if (func >= TUYA_PWM0 && func <= TUYA_PWM11) {
        __tkl_pwm_set_pin((TUYA_PWM_NUM_E)(func - TUYA_PWM0), func,
                          gpio_port, gpio_pin, af);
        return;
    }
    if (func >= TUYA_DVP0_D0 && func <= TUYA_DVP0_VSYNC) {
        __tkl_dvp_set_pin(func, gpio_port, gpio_pin, af);
        return;
    }
    if (func >= TUYA_SDIO_CLK && func <= TUYA_SDIO_DATA3) {
        __tkl_sdio_set_pin(func, gpio_port, gpio_pin, af);
        return;
    }
    if (func >= TUYA_ADC0 && func <= TUYA_ADC5) {
        __tkl_adc_set_pin((TUYA_ADC_NUM_E)(func - TUYA_ADC0), channel,
                          gpio_port, gpio_pin);
    }
}

static OPERATE_RET __apply(uint32_t pin_id, TUYA_PIN_FUNC_E func)
{
    uint32_t port, pad, rcu;
    const PIN_ROUTE_T *route = NULL;

    if (!__decode(pin_id, &port, &pad, &rcu)) {
        return OPRT_INVALID_PARM;
    }
    if (func != TUYA_GPIO && !__route_find(pin_id, func, &route)) {
        return OPRT_NOT_SUPPORTED;
    }
    rcu_periph_clock_enable(rcu);
    if (func == TUYA_GPIO) {
        gpio_mode_set(port, GPIO_MODE_INPUT, GPIO_PUPD_NONE, pad);
        s_last_valid[pin_id] = 0U;
        return OPRT_OK;
    }

    if (route->mode == MODE_ANALOG) {
        gpio_mode_set(port, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, pad);
    } else {
        gpio_af_set(port, s_afs[route->af], pad);
        gpio_mode_set(port, GPIO_MODE_AF,
                      route->mode == MODE_OD ? GPIO_PUPD_PULLUP : GPIO_PUPD_NONE,
                      pad);
        gpio_output_options_set(port,
                                route->mode == MODE_OD ? GPIO_OTYPE_OD : GPIO_OTYPE_PP,
                                GPIO_OSPEED_85MHZ, pad);
    }
    s_last_func[pin_id] = func;
    s_last_valid[pin_id] = 1U;
    __update_driver_pin(func, port, pad, s_afs[route->af], route->channel);
    return OPRT_OK;
}

OPERATE_RET tkl_io_pinmux_config(TUYA_PIN_NAME_E pin, TUYA_PIN_FUNC_E pin_func)
{
    if (pin >= TUYA_GPIO_NUM_MAX || !__decode(pin, &(uint32_t){0},
                                              &(uint32_t){0}, &(uint32_t){0})) {
        return OPRT_INVALID_PARM;
    }
    if (pin_func != TUYA_GPIO && !__route_find(pin, pin_func, NULL)) {
        return OPRT_NOT_SUPPORTED;
    }
    return __apply(pin, pin_func);
}

OPERATE_RET tkl_multi_io_pinmux_config(TUYA_MUL_PIN_CFG_T *cfg, uint16_t num)
{
    if (cfg == NULL || num == 0U) {
        return OPRT_INVALID_PARM;
    }
    /* Validate the entire batch, including duplicate pins, before changing HW. */
    for (uint16_t i = 0; i < num; ++i) {
        if (cfg[i].pin >= TUYA_GPIO_NUM_MAX ||
            !__decode(cfg[i].pin, &(uint32_t){0}, &(uint32_t){0}, &(uint32_t){0}) ||
            (cfg[i].pin_func != TUYA_GPIO &&
             !__route_find(cfg[i].pin, cfg[i].pin_func, NULL))) {
            return OPRT_INVALID_PARM;
        }
        for (uint16_t j = 0; j < i; ++j) {
            if (cfg[i].pin == cfg[j].pin) {
                return OPRT_INVALID_PARM;
            }
        }
    }
    for (uint16_t i = 0; i < num; ++i) {
        OPERATE_RET rt = __apply(cfg[i].pin, cfg[i].pin_func);
        if (rt != OPRT_OK) {
            return rt;
        }
    }
    return OPRT_OK;
}

static uint32_t __func_type(TUYA_PIN_FUNC_E func, TUYA_PIN_TYPE_E *type,
                            uint32_t *controller, uint32_t *channel)
{
    if (func == TUYA_GPIO) {
        *type = TUYA_IO_TYPE_GPIO;
        return 1U;
    }
    if (func <= TUYA_IIC5_SDA) {
        *type = TUYA_IO_TYPE_I2C;
        *controller = func / 2U;
        *channel = func % 2U;
        return 1U;
    }
    if (func >= TUYA_UART0_TX && func <= TUYA_UART7_CTS) {
        *type = TUYA_IO_TYPE_UART;
        *controller = (func - TUYA_UART0_TX) / 4U;
        *channel = (func - TUYA_UART0_TX) % 4U;
        return 1U;
    }
    if (func >= TUYA_QSPI0_SCK && func <= TUYA_QSPI0_CS) {
        *type = TUYA_IO_TYPE_QSPI;
        *controller = (func - TUYA_QSPI0_SCK) / 6U;
        *channel = (func - TUYA_QSPI0_SCK) % 6U;
        return 1U;
    }
    if ((func >= TUYA_SPI0_MISO && func <= TUYA_SPI2_CS) ||
        (func >= TUYA_SPI3_MISO && func <= TUYA_SPI5_CS)) {
        *type = TUYA_IO_TYPE_SPI;
        if (func <= TUYA_SPI2_CS) {
            *controller = (func - TUYA_SPI0_MISO) / 4U;
            *channel = (func - TUYA_SPI0_MISO) % 4U;
        } else {
            *controller = (func - TUYA_SPI3_MISO) / 4U + 3U;
            *channel = (func - TUYA_SPI3_MISO) % 4U;
        }
        return 1U;
    }
    if (func >= TUYA_PWM0 && func <= TUYA_PWM11) {
        *type = TUYA_IO_TYPE_PWM;
        *controller = 0U;
        *channel = func - TUYA_PWM0;
        return 1U;
    }
    if (func >= TUYA_DVP0_D0 && func <= TUYA_DVP0_VSYNC) {
        *type = TUYA_IO_TYPE_DVP;
        *controller = 0U;
        *channel = func - TUYA_DVP0_D0;
        return 1U;
    }
    if (func >= TUYA_ADC0 && func <= TUYA_ADC5) {
        *type = TUYA_IO_TYPE_ADC;
        *controller = func - TUYA_ADC0;
        *channel = 0U; /* Resolved from the matching route entry. */
        return 1U;
    }
    return 0U;
}

int32_t tkl_io_pin_to_func(uint32_t pin, TUYA_PIN_TYPE_E pin_type)
{
    uint32_t port, pad, rcu;
    TUYA_PIN_TYPE_E type;
    uint32_t controller, channel;
    uint16_t preferred = TUYA_PIN_FUNC_MAX;

    if (!__decode(pin, &port, &pad, &rcu) || pin_type >= TUYA_IO_TYPE_MAX) {
        return OPRT_INVALID_PARM;
    }
    if (s_last_valid[pin] && s_last_func[pin] != TUYA_GPIO) {
        preferred = s_last_func[pin];
        if (__func_type(preferred, &type, &controller, &channel) && type == pin_type) {
            const PIN_ROUTE_T *route = NULL;
            if (type == TUYA_IO_TYPE_ADC &&
                __route_find(pin, preferred, &route)) {
                channel = route->channel;
            }
            return (int32_t)((controller << 8) | channel);
        }
    }
    if (pin_type == TUYA_IO_TYPE_GPIO) {
        static const uint32_t bases[] = {GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOJ, GPIOK};
        uint32_t port_index;
        for (port_index = 0; port_index < sizeof(bases) / sizeof(bases[0]); ++port_index) {
            if (port == bases[port_index]) break;
        }
        return (int32_t)((port_index << 8) | __builtin_ctz(pad));
    }
    uint32_t best = UINT32_MAX;
    for (uint32_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        if (s_routes[i].pin != pin ||
            !__func_type(s_routes[i].func, &type, &controller, &channel) ||
            type != pin_type) {
            continue;
        }
        if (type == TUYA_IO_TYPE_ADC) {
            channel = s_routes[i].channel;
        }
        uint32_t candidate = (controller << 8) | channel;
        if (candidate < best) best = candidate;
    }
    return best == UINT32_MAX ? OPRT_NOT_SUPPORTED : (int32_t)best;
}
