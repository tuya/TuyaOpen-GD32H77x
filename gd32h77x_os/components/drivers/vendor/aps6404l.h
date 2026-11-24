/*!
    \file    aps6404l.h
    \brief   OSPI(quad) PSRAM APS6404L-3SQR (8MB, 64Mbit) driver for GD32H77x

    Hardware connection (OSPIM port0, OSPI0 by default, AF checked against the GD32H779 datasheet):
        PE11  --> CE#   (OSPIM_P0_NCS,  AF11)
        PB2   --> SCLK  (OSPIM_P0_CLK,  AF9)
        PA2   --> SIO0  (OSPIM_P0_IO0,  AF6)
        PB0   --> SIO1  (OSPIM_P0_IO1,  AF4)
        PA3   --> SIO2  (OSPIM_P0_IO2,  AF6)
        PA1   --> SIO3  (OSPIM_P0_IO3,  AF9)

    Typical usage:
        aps6404_dev_struct psram;
        if(SUCCESS == aps6404_init(&psram, OSPI0, ospi_kernel_clk_hz, APS6404_MODE_QPI)) {
            aps6404_write(&psram, 0, buf, len);       // indirect mode
            aps6404_read(&psram, 0, buf, len);
            aps6404_memory_map_enable(&psram);        // then access 0x90000000 ~ 0x907FFFFF
        }
*/

#ifndef APS6404L_H
#define APS6404L_H

#include "gd32h77x.h"
#include "gd32h77x_ospi.h"
#include "gd32h77x_ospim.h"

/* ---------------------------------------------------------------- */
/* pin configuration (change AF value here if the board is re-routed) */
/* ---------------------------------------------------------------- */
#define APS6404_CE_PORT             GPIOE
#define APS6404_CE_PIN              GPIO_PIN_11
#define APS6404_CE_AF               GPIO_AF_11

#define APS6404_SCK_PORT            GPIOB
#define APS6404_SCK_PIN             GPIO_PIN_2
#define APS6404_SCK_AF              GPIO_AF_9

#define APS6404_IO0_PORT            GPIOA
#define APS6404_IO0_PIN             GPIO_PIN_2
#define APS6404_IO0_AF              GPIO_AF_6

#define APS6404_IO1_PORT            GPIOB
#define APS6404_IO1_PIN             GPIO_PIN_0
#define APS6404_IO1_AF              GPIO_AF_4

#define APS6404_IO2_PORT            GPIOA
#define APS6404_IO2_PIN             GPIO_PIN_3
#define APS6404_IO2_AF              GPIO_AF_6

#define APS6404_IO3_PORT            GPIOA
#define APS6404_IO3_PIN             GPIO_PIN_1
#define APS6404_IO3_AF              GPIO_AF_9

/* ---------------------------------------------------------------- */
/* device parameters                                                */
/* ---------------------------------------------------------------- */
#define APS6404_SIZE_BYTES          0x00800000U     /* 8MB, A[22:0] */
#define APS6404_PAGE_SIZE           1024U           /* page size, burst never crosses it in indirect mode */

#define APS6404_SCK_MAX_HZ          84000000U       /* linear burst limit (also valid across page boundary) */
#define APS6404_SCK_ID_MAX_HZ       33000000U       /* Read ID(0x9F) limit */
#define APS6404_TCEM_NS             8000U           /* max CE# low time: 8us standard grade, 4000 for 3SQRX (extended) */

#define APS6404_MF_ID               0x0DU           /* AP Memory manufacturer ID */
#define APS6404_KGD_PASS            0x5DU           /* known good die */

/* memory mapped base address */
#define APS6404_MMAP_BASE(ospi)     (((ospi) == OSPI0) ? 0x90000000U : 0x70000000U)

/* commands, see datasheet 8.5 */
#define APS6404_CMD_READ            0x03U
#define APS6404_CMD_FAST_READ       0x0BU
#define APS6404_CMD_FAST_READ_QUAD  0xEBU
#define APS6404_CMD_WRITE           0x02U
#define APS6404_CMD_QUAD_WRITE      0x38U
#define APS6404_CMD_ENTER_QUAD      0x35U
#define APS6404_CMD_EXIT_QUAD       0xF5U
#define APS6404_CMD_RESET_ENABLE    0x66U
#define APS6404_CMD_RESET           0x99U
#define APS6404_CMD_WRAP_TOGGLE     0xC0U
#define APS6404_CMD_READ_ID         0x9FU

#define APS6404_QUAD_READ_DUMMY     6U              /* wait cycles of 0xEB */

typedef enum {
    APS6404_MODE_SPI = 0,                           /* cmd on 1 line, address/data on 4 lines (0xEB/0x38) */
    APS6404_MODE_QPI                                /* cmd/address/data all on 4 lines */
} aps6404_mode_enum;

typedef struct {
    uint8_t  mf_id;                                 /* manufacturer id, 0x0D */
    uint8_t  kgd;                                   /* known good die, 0x5D = pass */
    uint8_t  eid[6];                                /* eid[0] bit[7:5] is density, 0b010 = 64Mbit */
} aps6404_id_struct;

typedef struct {
    uint32_t               ospi_periph;             /* OSPI0 or OSPI1 */
    uint32_t               ospi_clk_hz;             /* OSPI kernel clock */
    uint32_t               sck_hz;                  /* actual SCK frequency */
    aps6404_mode_enum      mode;                    /* current PSRAM interface mode */
    ospi_parameter_struct  ospi;                    /* OSPI parameters used by the driver */
} aps6404_dev_struct;

/* initialize GPIO/OSPIM/OSPI, reset PSRAM, check ID and switch to the requested mode.
   ospi_clk_hz is the OSPI kernel clock selected by RCU_CFG4_OSPISEL (default CK_AHB) */
ErrStatus aps6404_init(aps6404_dev_struct *dev, uint32_t ospi_periph, uint32_t ospi_clk_hz, aps6404_mode_enum mode);
/* software reset (works in both SPI and QPI mode), PSRAM returns to SPI mode */
void aps6404_reset(aps6404_dev_struct *dev);
/* read ID, only available in SPI mode */
ErrStatus aps6404_read_id(aps6404_dev_struct *dev, aps6404_id_struct *id);
/* switch the PSRAM to QPI mode (0x35) / back to SPI mode (0xF5) */
void aps6404_enter_qpi(aps6404_dev_struct *dev);
void aps6404_exit_qpi(aps6404_dev_struct *dev);
/* indirect mode access, address is offset inside the PSRAM (0 ~ 0x7FFFFF) */
ErrStatus aps6404_write(aps6404_dev_struct *dev, uint32_t addr, const uint8_t *pdata, uint32_t size);
ErrStatus aps6404_read(aps6404_dev_struct *dev, uint32_t addr, uint8_t *pdata, uint32_t size);
/* enter / leave memory mapped mode, mapped address = APS6404_MMAP_BASE(ospi) + offset */
ErrStatus aps6404_memory_map_enable(aps6404_dev_struct *dev);
void aps6404_memory_map_disable(aps6404_dev_struct *dev);
/* simple indirect read/write test over several addresses, including a page boundary */
ErrStatus aps6404_selftest(aps6404_dev_struct *dev);

#endif /* APS6404L_H */
