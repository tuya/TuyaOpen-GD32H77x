#ifndef GD32H77X_78X_H
#define GD32H77X_78X_H

#include <stdint.h>

typedef enum {
    SPI0_IRQn = 0,
    SPI1_IRQn,
    SPI2_IRQn,
    SPI3_IRQn,
    SPI4_IRQn,
    SPI5_IRQn,
} IRQn_Type;

extern unsigned fake_nvic_enabled[6];
extern unsigned fake_nvic_pending[6];

static inline void NVIC_DisableIRQ(IRQn_Type irq)
{
    fake_nvic_enabled[irq] = 0U;
}

static inline void NVIC_EnableIRQ(IRQn_Type irq)
{
    fake_nvic_enabled[irq] = 1U;
}

static inline void NVIC_ClearPendingIRQ(IRQn_Type irq)
{
    fake_nvic_pending[irq] = 0U;
}

static inline void NVIC_SetPriority(IRQn_Type irq, unsigned int priority)
{
    (void)irq;
    (void)priority;
}

#define GPIOA 0U
#define GPIOB 1U
#define GPIOC 2U
#define GPIOD 3U
#define GPIOE 4U
#define GPIOF 5U
#define GPIOG 6U
#define GPIOH 7U
#define GPIOJ 8U
#define GPIOK 9U

#define RCU_GPIOA 10U
#define RCU_GPIOB 11U
#define RCU_GPIOC 12U
#define RCU_GPIOD 13U
#define RCU_GPIOE 14U
#define RCU_GPIOF 15U
#define RCU_GPIOG 16U
#define RCU_GPIOH 17U
#define RCU_GPIOJ 18U
#define RCU_GPIOK 19U

#define GPIO_PIN_0 (1U << 0)
#define GPIO_PIN_1 (1U << 1)
#define GPIO_PIN_2 (1U << 2)
#define GPIO_PIN_3 (1U << 3)
#define GPIO_PIN_4 (1U << 4)
#define GPIO_PIN_5 (1U << 5)
#define GPIO_PIN_6 (1U << 6)
#define GPIO_PIN_7 (1U << 7)
#define GPIO_PIN_8 (1U << 8)
#define GPIO_PIN_9 (1U << 9)
#define GPIO_PIN_10 (1U << 10)
#define GPIO_PIN_11 (1U << 11)
#define GPIO_PIN_12 (1U << 12)
#define GPIO_PIN_13 (1U << 13)
#define GPIO_PIN_14 (1U << 14)
#define GPIO_PIN_15 (1U << 15)

#define GPIO_MODE_AF 2U
#define GPIO_PUPD_NONE 0U
#define GPIO_OTYPE_PP 0U
#define GPIO_OSPEED_85MHZ 2U
#define GPIO_AF_0 0U
#define GPIO_AF_1 1U
#define GPIO_AF_2 2U
#define GPIO_AF_3 3U
#define GPIO_AF_4 4U
#define GPIO_AF_5 5U
#define GPIO_AF_6 6U
#define GPIO_AF_7 7U
#define GPIO_AF_8 8U
#define GPIO_AF_9 9U
#define GPIO_AF_10 10U
#define GPIO_AF_11 11U
#define GPIO_AF_12 12U
#define GPIO_AF_13 13U
#define GPIO_AF_14 14U
#define GPIO_AF_15 15U

#define RCU_SPI0 0U
#define RCU_SPI1 1U
#define RCU_SPI2 2U
#define RCU_SPI3 3U
#define RCU_SPI4 4U
#define RCU_SPI5 5U

#define RCU_SPISRC_PLL0Q 1U
#define RCU_SPISRC_APB2 2U
#define CK_PLL0Q 1U
#define CK_APB2 2U

#define RESET 0U
#define SET 1U

void rcu_spi_clock_config(uint32_t spi_idx, uint32_t clock_source);
void rcu_periph_clock_enable(uint32_t rcu);
uint32_t rcu_clock_freq_get(uint32_t clock);
void gpio_af_set(uint32_t port, uint32_t af, uint32_t pin);
void gpio_mode_set(uint32_t port, uint32_t mode, uint32_t pupd, uint32_t pin);
void gpio_output_options_set(uint32_t port, uint32_t type, uint32_t speed,
                             uint32_t pin);

#endif
