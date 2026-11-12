#ifndef GD32H77X_78X_EXTI_H
#define GD32H77X_78X_EXTI_H

#include "gd32h77x_78x.h"

typedef enum {
    EXTI_0 = 0,
    EXTI_1,
    EXTI_2,
    EXTI_3,
    EXTI_4,
    EXTI_5,
    EXTI_6,
    EXTI_7,
    EXTI_8,
    EXTI_9,
    EXTI_10,
    EXTI_11,
    EXTI_12,
    EXTI_13,
    EXTI_14,
    EXTI_15,
} exti_line_enum;

typedef enum {
    EXTI_INTERRUPT = 0,
    EXTI_EVENT,
} exti_mode_enum;

typedef enum {
    EXTI_TRIG_RISING = 0,
    EXTI_TRIG_FALLING,
    EXTI_TRIG_BOTH,
    EXTI_TRIG_NONE,
} exti_trig_type_enum;

void exti_init(exti_line_enum line, exti_mode_enum mode,
               exti_trig_type_enum trigger);
void exti_interrupt_enable(exti_line_enum line);
void exti_interrupt_disable(exti_line_enum line);
void exti_interrupt_flag_clear(exti_line_enum line);
FlagStatus exti_interrupt_flag_get(exti_line_enum line);

#endif
