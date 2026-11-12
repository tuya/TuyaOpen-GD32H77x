#ifndef SPI_AT_FAKE_WRAPPER_OS_H
#define SPI_AT_FAKE_WRAPPER_OS_H

#include <stdint.h>

typedef struct { unsigned count; } os_sema_t;
typedef struct { int unused; } os_mutex_t;
typedef void *os_task_t;

#define OS_OK 0
#define OS_TIMEOUT (-1)
#define OS_TASK_PRIORITY(n) (n)

uint32_t sys_current_time_get(void);
void sys_ms_sleep(uint32_t);
int sys_mutex_init(os_mutex_t *);
void sys_mutex_get(os_mutex_t *);
void sys_mutex_put(os_mutex_t *);
int sys_sema_init(os_sema_t *, unsigned);
int32_t sys_sema_down(os_sema_t *, int32_t);
void sys_sema_up(os_sema_t *);
os_task_t sys_task_create_dynamic(const uint8_t *, uint32_t, uint32_t,
                                  void (*)(void *), void *);
void sys_task_delete(void *);
void sys_enter_critical(void);
void sys_exit_critical(void);

#endif
