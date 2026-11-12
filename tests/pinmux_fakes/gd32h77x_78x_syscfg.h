#ifndef GD32H77X_78X_SYSCFG_H
#define GD32H77X_78X_SYSCFG_H

#include <stdint.h>

#define EXTI_SOURCE_GPIOA 0U
#define EXTI_SOURCE_GPIOB 1U
#define EXTI_SOURCE_GPIOC 2U
#define EXTI_SOURCE_GPIOD 3U
#define EXTI_SOURCE_GPIOE 4U
#define EXTI_SOURCE_GPIOF 5U
#define EXTI_SOURCE_GPIOG 6U
#define EXTI_SOURCE_GPIOH 7U
#define EXTI_SOURCE_GPIOJ 9U
#define EXTI_SOURCE_GPIOK 10U

void syscfg_exti_line_config(uint8_t port, uint8_t pin);

#endif
