#ifndef GD32H77X_78X_SPI_H
#define GD32H77X_78X_SPI_H

#include <stdint.h>

#define SPI0 0U
#define SPI1 1U
#define SPI2 2U
#define SPI3 3U
#define SPI4 4U
#define SPI5 5U

#define SPI_FLAG_RP      (1U << 0)
#define SPI_FLAG_TP      (1U << 1)
#define SPI_FLAG_ET      (1U << 3)
#define SPI_FLAG_TXURERR (1U << 5)
#define SPI_FLAG_RXORERR (1U << 6)
#define SPI_FLAG_CONFERR (1U << 9)
#define SPI_FLAG_ERRORS  (SPI_FLAG_TXURERR | SPI_FLAG_RXORERR | SPI_FLAG_CONFERR)

#define SPI_CTL0_MSTART (1U << 9)
#define SPI_CTL0_MSPDR  (1U << 10)

#define SPI_STATC_ETC       SPI_FLAG_ET
#define SPI_STATC_TXURERRC  SPI_FLAG_TXURERR
#define SPI_STATC_RXORERRC  SPI_FLAG_RXORERR
#define SPI_STATC_CONFERRC  SPI_FLAG_CONFERR
#define SPI_STATC_SPDC      (1U << 11)

#define SPI_INT_RPIE      (1U << 0)
#define SPI_INT_TPIE      (1U << 1)
#define SPI_INT_ESTCIE    (1U << 3)
#define SPI_INT_TXUREIE   (1U << 5)
#define SPI_INT_RXOREIE   (1U << 6)
#define SPI_INT_CONFEIE   (1U << 9)
#define SPI_INT_COMPLETE_MASK (SPI_INT_RPIE | SPI_INT_TPIE | SPI_INT_ESTCIE | \
                               SPI_INT_TXUREIE | SPI_INT_RXOREIE | SPI_INT_CONFEIE)

#define SPI_I2S_INT_RP    SPI_INT_RPIE
#define SPI_I2S_INT_TP    SPI_INT_TPIE
#define SPI_I2S_INT_ESTC  SPI_INT_ESTCIE
#define SPI_I2S_INT_TXURE SPI_INT_TXUREIE
#define SPI_I2S_INT_RXORE SPI_INT_RXOREIE
#define SPI_I2S_INT_CONFE SPI_INT_CONFEIE

#define SPI_TRANSMODE_FULLDUPLEX 0U
#define SPI_MASTER               0U
#define SPI_SLAVE                1U
#define SPI_DATASIZE_8BIT        0U
#define SPI_DATASIZE_16BIT       1U
#define SPI_CK_PL_LOW_PH_1EDGE   0U
#define SPI_CK_PL_LOW_PH_2EDGE   1U
#define SPI_CK_PL_HIGH_PH_1EDGE  2U
#define SPI_CK_PL_HIGH_PH_2EDGE  3U
#define SPI_NSS_HARD             1U
#define SPI_NSS_SOFT             0U
#define SPI_PSC_2                0U
#define SPI_PSC_4                1U
#define SPI_PSC_8                2U
#define SPI_PSC_16               3U
#define SPI_PSC_32               4U
#define SPI_PSC_64               5U
#define SPI_PSC_128              6U
#define SPI_PSC_256              7U
#define SPI_ENDIAN_LSB           1U
#define SPI_ENDIAN_MSB           0U
#define SPI_NSS_HOLD_UNTIL_TRANS_END 0U
#define SPI_NSS_POLARITY_LOW     0U
#define SPI_TRANS_START          1U

typedef struct {
    uint32_t trans_mode;
    uint32_t device_mode;
    uint32_t data_size;
    uint32_t clock_polarity_phase;
    uint32_t nss;
    uint32_t prescale;
    uint32_t endian;
} spi_parameter_struct;

typedef enum {
    SPI0_IDX = 0,
    SPI1_IDX,
    SPI2_IDX,
    SPI3_IDX,
    SPI4_IDX,
    SPI5_IDX,
} spi_idx_enum;

extern uint32_t fake_stat[6];
extern uint32_t fake_ctl0[6];
extern uint32_t fake_statc[6];
extern uint32_t fake_tdata[6];
extern uint32_t fake_rdata[6];
extern uint32_t fake_int[6];
extern uint32_t fake_spi_frames[6];
extern unsigned int fake_spi_disable_count[6];
extern unsigned int fake_spi_statc_write_count[6];
extern unsigned int fake_spi_suspend_probe_count[6];

#define SPI_STAT(p)  fake_stat[(p)]
#define SPI_CTL0(p)  (*fake_spi_ctl0_reg((p)))
#define SPI_STATC(p) (*fake_spi_statc_reg((p)))
#define SPI_TDATA(p) (*fake_spi_tdata_reg((p)))
#define SPI_RDATA(p) fake_spi_rdata_read((p))

uint32_t *fake_spi_ctl0_reg(uint32_t periph);
uint32_t *fake_spi_statc_reg(uint32_t periph);
uint32_t *fake_spi_tdata_reg(uint32_t periph);
uint32_t fake_spi_rdata_read(uint32_t periph);
void fake_spi_reset(uint32_t periph);

void spi_disable(uint32_t periph);
void spi_enable(uint32_t periph);
void spi_struct_para_init(spi_parameter_struct *cfg);
void spi_byte_access_enable(uint32_t periph);
void spi_nss_output_control(uint32_t periph, uint32_t control);
void spi_nss_polarity_set(uint32_t periph, uint32_t polarity);
void spi_nss_output_enable(uint32_t periph);
void spi_nss_output_disable(uint32_t periph);
void spi_nss_internal_high(uint32_t periph);
void spi_init(uint32_t periph, const spi_parameter_struct *cfg);
void spi_current_data_num_config(uint32_t periph, uint32_t frames);
void spi_master_transfer_start(uint32_t periph, uint32_t start);
void spi_i2s_interrupt_disable(uint32_t periph, uint32_t interrupts);
void spi_i2s_interrupt_enable(uint32_t periph, uint32_t interrupts);

void SPI0_IRQHandler(void);
void SPI1_IRQHandler(void);
void SPI2_IRQHandler(void);
void SPI3_IRQHandler(void);
void SPI4_IRQHandler(void);
void SPI5_IRQHandler(void);

#endif
