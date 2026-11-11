#ifndef TKL_SYSTEM_H
#define TKL_SYSTEM_H

#include "tuya_cloud_types.h"

extern unsigned int fake_critical_depth;
extern unsigned int fake_critical_entries;
extern unsigned int fake_critical_exits;
extern SYS_TIME_T fake_time_ms;

uint32_t tkl_system_enter_critical(void);
void tkl_system_exit_critical(uint32_t irq_mask);
SYS_TIME_T tkl_system_get_millisecond(void);

#endif
