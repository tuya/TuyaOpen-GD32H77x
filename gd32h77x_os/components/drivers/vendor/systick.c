#include "systick.h"
#include "FreeRTOS.h"
#include "task.h"

void delay_1ms(uint32_t count)
{
    vTaskDelay(pdMS_TO_TICKS(count));
}
