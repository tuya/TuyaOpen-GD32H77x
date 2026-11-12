#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "tkl_gpio.h"
#include "tkl_pinmux.h"
#include "gd32h77x_78x.h"
#include "gd32h77x_78x_exti.h"

uint32_t fake_bop[10];
uint32_t fake_bc[10];
static unsigned int call_count;
static uint32_t last_port, last_pin, last_af, last_mode, last_pupd, last_type;

void tkl_log_output(const char *format, ...) { (void)format; }
void NVIC_EnableIRQ(IRQn_Type irq) { (void)irq; }
void NVIC_DisableIRQ(IRQn_Type irq) { (void)irq; }
void NVIC_ClearPendingIRQ(IRQn_Type irq) { (void)irq; }
void NVIC_SetPriority(IRQn_Type irq, uint32_t priority) { (void)irq; (void)priority; }
void syscfg_exti_line_config(uint8_t port, uint8_t pin) { (void)port; (void)pin; }
void exti_init(exti_line_enum line, exti_mode_enum mode, exti_trig_type_enum trigger)
{
    (void)line; (void)mode; (void)trigger;
}
void exti_interrupt_enable(exti_line_enum line) { (void)line; }
void exti_interrupt_disable(exti_line_enum line) { (void)line; }
void exti_interrupt_flag_clear(exti_line_enum line) { (void)line; }
FlagStatus exti_interrupt_flag_get(exti_line_enum line) { (void)line; return RESET; }

void __tkl_spi_set_pin(TUYA_SPI_NUM_E port, TUYA_PIN_FUNC_E func,
                       uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    (void)port; (void)func; (void)gpio_port; (void)gpio_pin; (void)af;
}
void __tkl_qspi_set_pin(TUYA_QSPI_NUM_E port, TUYA_PIN_FUNC_E func,
                        uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    (void)port; (void)func; (void)gpio_port; (void)gpio_pin; (void)af;
}
void __tkl_i2c_set_pin(TUYA_I2C_NUM_E port, TUYA_PIN_FUNC_E func,
                       uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    (void)port; (void)func; (void)gpio_port; (void)gpio_pin; (void)af;
}
void __tkl_uart_set_pin(TUYA_UART_NUM_E port, TUYA_PIN_FUNC_E func,
                        uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    (void)port; (void)func; (void)gpio_port; (void)gpio_pin; (void)af;
}
void __tkl_dvp_set_pin(TUYA_PIN_FUNC_E func, uint32_t gpio_port,
                       uint32_t gpio_pin, uint32_t af)
{
    (void)func; (void)gpio_port; (void)gpio_pin; (void)af;
}
void __tkl_adc_set_pin(TUYA_ADC_NUM_E port, uint8_t channel,
                       uint32_t gpio_port, uint32_t gpio_pin)
{
    (void)port; (void)channel; (void)gpio_port; (void)gpio_pin;
}
void __tkl_pwm_set_pin(TUYA_PWM_NUM_E port, TUYA_PIN_FUNC_E func,
                       uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    (void)port; (void)func; (void)gpio_port; (void)gpio_pin; (void)af;
}
void __tkl_sdio_set_pin(TUYA_PIN_FUNC_E func, uint32_t gpio_port,
                        uint32_t gpio_pin, uint32_t af)
{
    (void)func; (void)gpio_port; (void)gpio_pin; (void)af;
}

void rcu_periph_clock_enable(uint32_t rcu) { (void)rcu; ++call_count; }
void gpio_af_set(uint32_t port, uint32_t af, uint32_t pin)
{
    ++call_count; last_port = port; last_pin = pin; last_af = af;
}
void gpio_mode_set(uint32_t port, uint32_t mode, uint32_t pupd, uint32_t pin)
{
    ++call_count; last_port = port; last_pin = pin; last_mode = mode; last_pupd = pupd;
}
void gpio_output_options_set(uint32_t port, uint32_t type, uint32_t speed, uint32_t pin)
{
    (void)speed; ++call_count; last_port = port; last_pin = pin; last_type = type;
}
uint32_t gpio_input_bit_get(uint32_t port, uint32_t pin)
{
    (void)port; (void)pin; return SET;
}

static void expect_single_no_change(uint32_t id, TUYA_PIN_FUNC_E func)
{
    unsigned int before = call_count;
    assert(tkl_io_pinmux_config((TUYA_PIN_NAME_E)id, func) == OPRT_NOT_SUPPORTED);
    assert(call_count == before);
}

static void test_gpio_table_is_stable(void)
{
    static const uint32_t ports[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE,
        GPIOF, GPIOG, GPIOH, GPIOJ, GPIOK,
    };
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };

    for (uint32_t id = 0; id < TUYA_GPIO_NUM_MAX; ++id) {
        uint32_t port_index = id / 16U;
        uint32_t pin_index = id % 16U;
        assert(tkl_gpio_init((TUYA_GPIO_NUM_E)id, &cfg) == OPRT_OK);
        assert(last_port == ports[port_index]);
        assert(last_pin == (1U << pin_index));
    }
    assert(tkl_gpio_init(TUYA_GPIO_NUM_MAX, &cfg) == OPRT_INVALID_PARM);
}

static void test_af_routes_and_rejections(void)
{
    call_count = 0;
    assert(tkl_io_pinmux_config(6, TUYA_SPI0_MISO) == OPRT_OK); /* PA6 AF5 */
    assert(last_port == GPIOA && last_pin == GPIO_PIN_6 && last_af == GPIO_AF_5);
    assert(tkl_io_pinmux_config(80, TUYA_SPI3_MISO) == OPRT_OK); /* PF0 AF1 */
    assert(last_port == GPIOF && last_pin == GPIO_PIN_0 && last_af == GPIO_AF_1);
    assert(last_mode == GPIO_MODE_AF && last_type == GPIO_OTYPE_PP);
    expect_single_no_change(80, TUYA_SPI4_MISO);

    assert(tkl_io_pinmux_config(76, TUYA_QSPI0_SCK) == OPRT_OK); /* PE12 AF5 */
    assert(last_port == GPIOE && last_pin == GPIO_PIN_12 && last_af == GPIO_AF_5);
    assert(tkl_io_pinmux_config(73, TUYA_QSPI0_IO2) == OPRT_OK); /* PE9 AF5 */
    assert(last_port == GPIOE && last_pin == GPIO_PIN_9 && last_af == GPIO_AF_5);
    expect_single_no_change(79, TUYA_QSPI0_IO1); /* PE15 has no SPI3_IO1 */

    assert(tkl_io_pinmux_config(86, TUYA_SPI4_CS) == OPRT_OK); /* PF6 AF5 */
    assert(last_port == GPIOF && last_pin == GPIO_PIN_6 && last_af == GPIO_AF_5);
    expect_single_no_change(83, TUYA_SPI4_CS); /* PF3 has no SPI4_NSS */

    assert(tkl_io_pinmux_config(24, TUYA_IIC0_SCL) == OPRT_OK); /* PB8 AF4 */
    assert(last_port == GPIOB && last_pin == GPIO_PIN_8 && last_af == GPIO_AF_4);
    assert(last_mode == GPIO_MODE_AF && last_type == GPIO_OTYPE_OD);

    assert(tkl_io_pinmux_config(9, TUYA_UART0_TX) == OPRT_OK); /* PA9 AF7 */
    assert(last_port == GPIOA && last_pin == GPIO_PIN_9 && last_af == GPIO_AF_7);
    assert(tkl_io_pin_to_func(9, TUYA_IO_TYPE_UART) == 0x0000U);
    assert(tkl_io_pinmux_config(2, TUYA_UART1_TX) == OPRT_OK); /* PA2 AF7 */
    assert(tkl_io_pin_to_func(2, TUYA_IO_TYPE_UART) == 0x0100U);
    assert(tkl_io_pinmux_config(38, TUYA_UART5_TX) == OPRT_OK); /* PC6 AF7 */
    assert(last_port == GPIOC && last_pin == GPIO_PIN_6 && last_af == GPIO_AF_7);
    assert(tkl_io_pin_to_func(38, TUYA_IO_TYPE_UART) == 0x0500U);
    assert(tkl_io_pinmux_config(72, TUYA_UART6_TX) == OPRT_OK); /* PE8 AF7 */
    assert(tkl_io_pin_to_func(72, TUYA_IO_TYPE_UART) == 0x0600U);
    assert(tkl_io_pinmux_config(65, TUYA_UART7_TX) == OPRT_OK); /* PE1 AF8 */
    assert(last_port == GPIOE && last_pin == GPIO_PIN_1 && last_af == GPIO_AF_8);
    assert(tkl_io_pin_to_func(65, TUYA_IO_TYPE_UART) == 0x0700U);

    assert(tkl_io_pinmux_config(8, TUYA_PWM0) == OPRT_OK); /* PA8 AF1 */
    assert(last_port == GPIOA && last_pin == GPIO_PIN_8 && last_af == GPIO_AF_1);
    assert(tkl_io_pin_to_func(8, TUYA_IO_TYPE_PWM) == 0x0000U);
    assert(tkl_io_pinmux_config(16, TUYA_PWM10) == OPRT_OK); /* PB0 AF2 */
    assert(tkl_io_pin_to_func(16, TUYA_IO_TYPE_PWM) == 0x000AU);

    assert(tkl_io_pinmux_config(114, TUYA_UART2_TX) == OPRT_OK); /* PH2 AF1 */
    assert(last_port == GPIOH && last_pin == GPIO_PIN_2 && last_af == GPIO_AF_1);
    assert(tkl_io_pinmux_config(116, TUYA_UART2_RX) == OPRT_OK); /* PH4 AF1 */
    expect_single_no_change(116, TUYA_UART2_TX);
    expect_single_no_change(118, TUYA_UART2_RX);

    assert(tkl_io_pinmux_config(23, TUYA_IIC3_SDA) == OPRT_OK); /* PB7 AF6 */
    assert(last_port == GPIOB && last_pin == GPIO_PIN_7 && last_af == GPIO_AF_6);
    assert(last_mode == GPIO_MODE_AF && last_pupd == GPIO_PUPD_PULLUP);
    assert(last_type == GPIO_OTYPE_OD);
    expect_single_no_change(79, TUYA_SPI3_MISO); /* PE15 has no SPI3 alternate function */

    assert(tkl_io_pinmux_config(32, TUYA_ADC0) == OPRT_OK); /* PC0 analog */
    assert(last_port == GPIOC && last_pin == GPIO_PIN_0);
    assert(last_mode == GPIO_MODE_ANALOG && last_pupd == GPIO_PUPD_NONE);
    assert(tkl_io_pin_to_func(32, TUYA_IO_TYPE_ADC) == 0x000AU);
    expect_single_no_change(13, TUYA_DVP0_D4); /* PD5 is not DCI_D4 */
    assert(tkl_io_pinmux_config(126, TUYA_DVP0_D4) == OPRT_OK); /* PH14 AF13 */
    assert(last_port == GPIOH && last_pin == GPIO_PIN_14 && last_af == GPIO_AF_13);
    assert(tkl_io_pin_to_func(126, TUYA_IO_TYPE_DVP) == 0x0004U);
    assert(tkl_io_pinmux_config(44, TUYA_SDIO_CLK) == OPRT_OK); /* PC12 AF12 */
    assert(last_port == GPIOC && last_pin == GPIO_PIN_12 && last_af == GPIO_AF_12);
    assert(tkl_io_pinmux_config(40, TUYA_SDIO_DATA0) == OPRT_OK); /* PC8 AF12 */
    assert(tkl_io_pinmux_config(29, TUYA_SDIO_DATA0) == OPRT_OK); /* PB13 AF12 */
    assert(last_port == GPIOB && last_pin == GPIO_PIN_13 && last_af == GPIO_AF_12);
    assert(tkl_io_pinmux_config(56, TUYA_SDIO_DATA2) == OPRT_OK); /* PD8 AF6 */
    assert(last_port == GPIOD && last_pin == GPIO_PIN_8 && last_af == GPIO_AF_6);
    assert(tkl_io_pinmux_config(57, TUYA_SDIO_DATA3) == OPRT_OK); /* PD9 AF6 */
    assert(last_port == GPIOD && last_pin == GPIO_PIN_9 && last_af == GPIO_AF_6);
    assert(tkl_io_pinmux_config(33, TUYA_SDIO_CLK) == OPRT_OK); /* PC1 AF9 */
    assert(last_port == GPIOC && last_pin == GPIO_PIN_1 && last_af == GPIO_AF_9);
    assert(tkl_io_pinmux_config(0, TUYA_SDIO_CMD) == OPRT_OK); /* PA0 AF9 */
    assert(last_port == GPIOA && last_pin == GPIO_PIN_0 && last_af == GPIO_AF_9);
    assert(tkl_io_pinmux_config(30, TUYA_SDIO_DATA0) == OPRT_OK); /* PB14 AF9 */
    assert(last_port == GPIOB && last_pin == GPIO_PIN_14 && last_af == GPIO_AF_9);
    assert(tkl_io_pinmux_config(29, TUYA_SDIO_DATA2) == OPRT_OK); /* PB13 AF16 */
    assert(last_port == GPIOB && last_pin == GPIO_PIN_13 && last_af == GPIO_AF_16);
    /* H77 MCLK is a fixed CKOUT0 output and is not a pinmux route. */
    expect_single_no_change(8, TUYA_DVP0_MCLK);
}

static void test_gpio_query_and_batch_preflight(void)
{
    TUYA_MUL_PIN_CFG_T batch[] = {
        {.pin = 76, .pin_func = TUYA_SPI3_CLK},
        {.pin = 15, .pin_func = TUYA_SPI3_MISO},
    };
    unsigned int before = call_count;
    assert(tkl_multi_io_pinmux_config(batch, 2) == OPRT_INVALID_PARM);
    assert(call_count == before); /* no first-pin register writes */
    assert(tkl_multi_io_pinmux_config(NULL, 1) == OPRT_INVALID_PARM);
    assert(tkl_multi_io_pinmux_config(batch, 0) == OPRT_INVALID_PARM);

    assert(tkl_io_pinmux_config(61, TUYA_GPIO) == OPRT_OK); /* PD13 as GPIO */
    assert(last_port == GPIOD && last_pin == GPIO_PIN_13);
    assert(last_mode == GPIO_MODE_INPUT);
    assert(tkl_io_pin_to_func(61, TUYA_IO_TYPE_GPIO) == 0x030DU);

    assert(tkl_io_pinmux_config(6, TUYA_SPI5_MISO) == OPRT_OK);
    assert(tkl_io_pin_to_func(6, TUYA_IO_TYPE_SPI) == 0x0500U);
    assert(tkl_io_pinmux_config(6, TUYA_SPI0_MISO) == OPRT_OK);
    assert(tkl_io_pin_to_func(6, TUYA_IO_TYPE_SPI) == 0x0000U);
    assert(tkl_io_pinmux_config(76, TUYA_SPI3_CLK) == OPRT_OK);
    assert(tkl_io_pin_to_func(76, TUYA_IO_TYPE_SPI) == 0x0302U);
    assert(tkl_io_pin_to_func(79, TUYA_IO_TYPE_SPI) == OPRT_NOT_SUPPORTED);

    TUYA_MUL_PIN_CFG_T qspi_batch[] = {
        {.pin = 76, .pin_func = TUYA_QSPI0_SCK},
        {.pin = 78, .pin_func = TUYA_QSPI0_IO0},
        {.pin = 77, .pin_func = TUYA_QSPI0_IO1},
        {.pin = 73, .pin_func = TUYA_QSPI0_IO2},
        {.pin = 74, .pin_func = TUYA_QSPI0_IO3},
        {.pin = 75, .pin_func = TUYA_QSPI0_CS},
    };
    assert(tkl_multi_io_pinmux_config(qspi_batch, 6) == OPRT_OK);
    assert(tkl_io_pin_to_func(76, TUYA_IO_TYPE_QSPI) == 0x0000U);
    assert(tkl_io_pin_to_func(78, TUYA_IO_TYPE_QSPI) == 0x0001U);
    assert(tkl_io_pin_to_func(77, TUYA_IO_TYPE_QSPI) == 0x0002U);
    assert(tkl_io_pin_to_func(73, TUYA_IO_TYPE_QSPI) == 0x0003U);
    assert(tkl_io_pin_to_func(74, TUYA_IO_TYPE_QSPI) == 0x0004U);
    assert(tkl_io_pin_to_func(75, TUYA_IO_TYPE_QSPI) == 0x0005U);
}

int main(void)
{
    test_gpio_table_is_stable();
    test_af_routes_and_rejections();
    test_gpio_query_and_batch_preflight();
    puts("GD32H77X pinmux host tests: PASS");
    return 0;
}
