#ifndef SPI_AT_FAKE_GD32VW55X_H
#define SPI_AT_FAKE_GD32VW55X_H

#include <stdint.h>

typedef enum { RESET = 0, SET = 1 } FlagStatus;
typedef struct {
    uint32_t periph_addr, periph_memory_width, priority, number;
    uint32_t periph_inc, memory_inc, circular_mode, direction, memory0_addr;
} dma_single_data_parameter_struct;
typedef struct {
    uint32_t trans_mode, device_mode, frame_size, clock_polarity_phase;
    uint32_t prescale, endian, nss;
} spi_parameter_struct;

#define GPIOA 1U
#define GPIO_PIN_0 (1U << 0)
#define GPIO_PIN_1 (1U << 1)
#define GPIO_PIN_2 (1U << 2)
#define GPIO_PIN_3 (1U << 3)
#define GPIO_PIN_4 (1U << 4)
#define GPIO_PIN_5 (1U << 5)
#define GPIO_AF_2 2U
#define GPIO_AF_4 4U
#define GPIO_AF_5 5U
#define GPIO_MODE_INPUT 0U
#define GPIO_MODE_OUTPUT 1U
#define GPIO_MODE_AF 2U
#define GPIO_PUPD_PULLDOWN 1U
#define GPIO_PUPD_PULLUP 2U
#define GPIO_PUPD_NONE 0U
#define GPIO_OTYPE_PP 0U
#define GPIO_OSPEED_25MHZ 25U
#define RCU_GPIOA 1U
#define RCU_SPI 2U
#define RCU_DMA 3U
#define SPI_RX_DMA_CH 4U
#define SPI_TX_DMA_CH 5U
#define SPI_RX_DMA_CH 4U
#define SPI_TX_DMA_CH 5U
#define DMA_INTF_FTFIF 1U
#define SPI_DMA_RECEIVE 1U
#define SPI_DMA_TRANSMIT 2U
#define SPI_FLAG_TRANS 1U
#define SPI_FLAG_TBE 2U
#define SPI_FLAG_RBNE 4U
#define SPI_FLAG_RXORERR 8U
#define SPI_TRANSMODE_FULLDUPLEX 1U
#define SPI_SLAVE 0U
#define SPI_FRAMESIZE_8BIT 8U
#define SPI_CK_PL_LOW_PH_1EDGE 0U
#define SPI_PSC_4 4U
#define SPI_ENDIAN_MSB 0U
#define SPI_NSS_SOFT 0U
#define SPI_NSS_HARD 1U
#define DMA_PERIPH_WIDTH_8BIT 8U
#define DMA_PRIORITY_HIGH 1U
#define DMA_PERIPH_INCREASE_DISABLE 0U
#define DMA_MEMORY_INCREASE_ENABLE 1U
#define DMA_CIRCULAR_MODE_DISABLE 0U
#define DMA_PERIPH_TO_MEMORY 0U
#define DMA_MEMORY_TO_PERIPH 1U
#define DMA_MEMORY_0 0U
#define DMA_SUBPERI3 3U
#define DMA_INT_FTF 1U
#define SPI_DATA (*(volatile uint32_t *)0x1000U)

void rcu_periph_clock_enable(uint32_t);
void gpio_af_set(uint32_t, uint32_t, uint32_t);
void gpio_mode_set(uint32_t, uint32_t, uint32_t, uint32_t);
void gpio_output_options_set(uint32_t, uint32_t, uint32_t, uint32_t);
void gpio_bit_reset(uint32_t, uint32_t);
void gpio_bit_set(uint32_t, uint32_t);
FlagStatus gpio_input_bit_get(uint32_t, uint32_t);
void spi_deinit(void);
void spi_struct_para_init(spi_parameter_struct *);
void spi_init(spi_parameter_struct *);
void spi_nss_internal_low(void);
void spi_enable(void);
void spi_disable(void);
FlagStatus spi_flag_get(uint32_t);
void spi_rx_flush(void);
void spi_crc_error_clear(void);
void spi_dma_enable(uint32_t);
void spi_dma_disable(uint32_t);
void dma_deinit(uint32_t);
void dma_single_data_mode_init(uint32_t, dma_single_data_parameter_struct *);
void dma_single_data_para_struct_init(dma_single_data_parameter_struct *);
void dma_channel_subperipheral_select(uint32_t, uint32_t);
void dma_interrupt_disable(uint32_t, uint32_t);
void dma_channel_disable(uint32_t);
void dma_flag_clear(uint32_t, uint32_t);
void dma_memory_address_config(uint32_t, uint32_t, uint32_t);
void dma_transfer_number_config(uint32_t, uint32_t);
void dma_memory_address_generation_config(uint32_t, uint32_t);
void dma_channel_enable(uint32_t);
FlagStatus dma_flag_get(uint32_t, uint32_t);

#endif
