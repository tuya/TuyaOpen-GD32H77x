/**
 * @file tuyaopen_license.h
 * @brief GD32H77x platform bring-up and FreeRTOS adaptation.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef TUYAOPEN_LICENSE_H
#define TUYAOPEN_LICENSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
int tuyaopen_license_write(const char *data, uint32_t data_len);
int tuyaopen_license_read(char **data, uint32_t *data_len);
#ifdef __cplusplus
}
#endif

#endif
