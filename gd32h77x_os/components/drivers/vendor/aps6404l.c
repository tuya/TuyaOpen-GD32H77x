/*!
    \file    aps6404l.c
    \brief   OSPI(quad) PSRAM APS6404L-3SQR (8MB, 64Mbit) driver for GD32H77x
*/

#include "aps6404l.h"
#include "systick.h"
#include <string.h>

#define APS6404_ID_SIZE             8U              /* MF ID + KGD + EID[47:0] */
#define APS6404_MIN_CHUNK           8U              /* smallest usable chunk, otherwise the SCK is too slow for tCEM */
#define APS6404_MMAP_TIMEOUT        0x20U           /* release CE# after 32 idle SCK cycles in memory mapped mode */

static void aps6404_pin_init(uint32_t port, uint32_t pin, uint32_t af, uint32_t pupd)
{
    gpio_af_set(port, af, pin);
    gpio_mode_set(port, GPIO_MODE_AF, pupd, pin);
    gpio_output_options_set(port, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ, pin);
}

static void aps6404_gpio_ospim_init(uint32_t ospi_periph)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_periph_clock_enable(RCU_OSPIM);

    /* CE# must stay high while idle */
    aps6404_pin_init(APS6404_CE_PORT, APS6404_CE_PIN, APS6404_CE_AF, GPIO_PUPD_PULLUP);
    aps6404_pin_init(APS6404_SCK_PORT, APS6404_SCK_PIN, APS6404_SCK_AF, GPIO_PUPD_NONE);
    aps6404_pin_init(APS6404_IO0_PORT, APS6404_IO0_PIN, APS6404_IO0_AF, GPIO_PUPD_NONE);
    aps6404_pin_init(APS6404_IO1_PORT, APS6404_IO1_PIN, APS6404_IO1_AF, GPIO_PUPD_NONE);
    aps6404_pin_init(APS6404_IO2_PORT, APS6404_IO2_PIN, APS6404_IO2_AF, GPIO_PUPD_NONE);
    aps6404_pin_init(APS6404_IO3_PORT, APS6404_IO3_PIN, APS6404_IO3_AF, GPIO_PUPD_NONE);

    /* route SCK, CSN and IO[3:0] of the selected OSPI to OSPIM port0 */
    psram_vendor_ospim_port_sck_config(OSPIM_PORT0, OSPIM_PORT_SCK_ENABLE);
    psram_vendor_ospim_port_csn_config(OSPIM_PORT0, OSPIM_PORT_CSN_ENABLE);
    psram_vendor_ospim_port_io3_0_config(OSPIM_PORT0, OSPIM_IO_LOW_ENABLE);
    if(OSPI0 == ospi_periph) {
        rcu_periph_clock_enable(RCU_OSPI0);
        psram_vendor_ospim_port_sck_source_select(OSPIM_PORT0, OSPIM_SCK_SOURCE_OSPI0_SCK);
        psram_vendor_ospim_port_csn_source_select(OSPIM_PORT0, OSPIM_CSN_SOURCE_OSPI0_CSN);
        psram_vendor_ospim_port_io3_0_source_select(OSPIM_PORT0, OSPIM_SRCPLIO_OSPI0_IO_LOW);
    } else {
        rcu_periph_clock_enable(RCU_OSPI1);
        psram_vendor_ospim_port_sck_source_select(OSPIM_PORT0, OSPIM_SCK_SOURCE_OSPI1_SCK);
        psram_vendor_ospim_port_csn_source_select(OSPIM_PORT0, OSPIM_CSN_SOURCE_OSPI1_CSN);
        psram_vendor_ospim_port_io3_0_source_select(OSPIM_PORT0, OSPIM_SRCPLIO_OSPI1_IO_LOW);
    }
}

/* number of SCK cycles that fit in tCEM */
static uint32_t aps6404_tcem_cycles(const aps6404_dev_struct *dev)
{
    return (uint32_t)(((uint64_t)dev->sck_hz * APS6404_TCEM_NS) / 1000000000ULL);
}

/* max bytes per indirect transfer so that CE# low time (command + address + wait + data)
   stays well below tCEM, half of tCEM is used as margin for FIFO stalls */
static uint32_t aps6404_chunk_limit(const aps6404_dev_struct *dev)
{
    uint32_t overhead = ((APS6404_MODE_QPI == dev->mode) ? 2U : 8U) + 6U + APS6404_QUAD_READ_DUMMY;
    uint32_t budget = aps6404_tcem_cycles(dev) / 2U;
    uint32_t limit;

    if(budget < (overhead + 2U * APS6404_MIN_CHUNK)) {
        return 0U;
    }
    /* quad: 2 SCK cycles per byte */
    limit = (budget - overhead) / 2U;
    return (limit > APS6404_PAGE_SIZE) ? APS6404_PAGE_SIZE : limit;
}

/* (re)configure OSPI so that SCK <= max_hz; refresh enables the CE# release for memory mapped mode */
static void aps6404_ospi_setup(aps6404_dev_struct *dev, uint32_t max_hz, ControlStatus refresh)
{
    uint32_t div = (dev->ospi_clk_hz + max_hz - 1U) / max_hz;

    if(0U == div) {
        div = 1U;
    } else if(div > 256U) {
        div = 256U;
    }
    dev->sck_hz = dev->ospi_clk_hz / div;

    psram_vendor_ospi_disable(dev->ospi_periph);
    psram_vendor_ospi_struct_init(&dev->ospi);
    dev->ospi.prescaler        = div - 1U;
    /* data is launched on the falling edge and tACLK is up to 5.5ns, sample half a cycle later */
    dev->ospi.sample_shift     = OSPI_SAMPLE_SHIFTING_HALF_CYCLE;
    dev->ospi.fifo_threshold   = OSPI_FIFO_THRESHOLD_4;
    dev->ospi.device_size      = OSPI_MESZ_8_MBS;
    dev->ospi.cs_hightime      = OSPI_CS_HIGH_TIME_3_CYCLE;     /* tCPH >= 18ns */
    dev->ospi.memory_type      = OSPI_STANDARD_MODE;
    dev->ospi.delay_hold_cycle = OSPI_DELAY_HOLD_NONE;
    dev->ospi.refrate          = (ENABLE == refresh) ? (aps6404_tcem_cycles(dev) / 2U) : 0U;
    psram_vendor_ospi_init(dev->ospi_periph, &dev->ospi);
    psram_vendor_ospi_enable(dev->ospi_periph);
}

static void aps6404_cmd_base(psram_vendor_ospi_regular_cmd_struct *cmd, uint32_t op_type, uint32_t ins_mode, uint8_t instruction)
{
    memset(cmd, 0, sizeof(psram_vendor_ospi_regular_cmd_struct));
    cmd->operation_type       = op_type;
    cmd->instruction          = instruction;
    cmd->ins_mode             = ins_mode;
    cmd->ins_size             = OSPI_INSTRUCTION_8_BITS;
    cmd->ins_dtr_mode         = OSPI_INSDTR_MODE_DISABLE;
    cmd->addr_mode            = OSPI_ADDRESS_NONE;
    cmd->addr_size            = OSPI_ADDRESS_24_BITS;
    cmd->addr_dtr_mode        = OSPI_ADDRDTR_MODE_DISABLE;
    cmd->alter_bytes_mode     = OSPI_ALTERNATE_BYTES_NONE;
    cmd->alter_bytes_dtr_mode = OSPI_ABDTR_MODE_DISABLE;
    cmd->data_mode            = OSPI_DATA_NONE;
    cmd->data_dtr_mode        = OSPI_DADTR_MODE_DISABLE;
    cmd->dummy_cycles         = OSPI_DUMYC_CYCLES_0;
    cmd->dqs_mode             = OSPI_DQS_DISABLE;
    cmd->sioo_mode            = OSPI_SIOO_INST_EVERY_CMD;
}

static uint32_t aps6404_ins_mode(const aps6404_dev_struct *dev)
{
    return (APS6404_MODE_QPI == dev->mode) ? OSPI_INSTRUCTION_4_LINES : OSPI_INSTRUCTION_1_LINE;
}

/* command without address and data */
static void aps6404_simple_cmd(aps6404_dev_struct *dev, uint8_t instruction, uint32_t ins_mode)
{
    psram_vendor_ospi_regular_cmd_struct cmd;

    aps6404_cmd_base(&cmd, OSPI_OPTYPE_COMMON_CFG, ins_mode, instruction);
    psram_vendor_ospi_command_config(dev->ospi_periph, &dev->ospi, &cmd);
}

/*!
    \brief      software reset, the PSRAM returns to SPI mode
    \note       the PSRAM may still be in QPI mode after an MCU reset, so the reset
                is issued with 4-line and 1-line commands. An incomplete 4-line command
                (2 clocks) is ignored by a device in SPI mode.
*/
void aps6404_reset(aps6404_dev_struct *dev)
{
    aps6404_simple_cmd(dev, APS6404_CMD_RESET_ENABLE, OSPI_INSTRUCTION_4_LINES);
    aps6404_simple_cmd(dev, APS6404_CMD_RESET, OSPI_INSTRUCTION_4_LINES);
    aps6404_simple_cmd(dev, APS6404_CMD_RESET_ENABLE, OSPI_INSTRUCTION_1_LINE);
    aps6404_simple_cmd(dev, APS6404_CMD_RESET, OSPI_INSTRUCTION_1_LINE);
    dev->mode = APS6404_MODE_SPI;
    /* tRST = 50ns */
    delay_1ms(1U);
}

/*!
    \brief      read the PSRAM ID (SPI mode only, max SCK 33MHz)
*/
ErrStatus aps6404_read_id(aps6404_dev_struct *dev, aps6404_id_struct *id)
{
    psram_vendor_ospi_regular_cmd_struct cmd;
    uint8_t buf[APS6404_ID_SIZE];

    if((NULL == dev) || (NULL == id) || (APS6404_MODE_SPI != dev->mode)) {
        return ERROR;
    }

    aps6404_cmd_base(&cmd, OSPI_OPTYPE_COMMON_CFG, OSPI_INSTRUCTION_1_LINE, APS6404_CMD_READ_ID);
    cmd.addr_mode = OSPI_ADDRESS_1_LINE;
    cmd.address   = 0U;
    cmd.data_mode = OSPI_DATA_1_LINE;
    cmd.nbdata    = APS6404_ID_SIZE;
    psram_vendor_ospi_command_config(dev->ospi_periph, &dev->ospi, &cmd);
    psram_vendor_ospi_receive(dev->ospi_periph, &dev->ospi, buf);

    id->mf_id = buf[0];
    id->kgd   = buf[1];
    memcpy(id->eid, &buf[2], sizeof(id->eid));
    return SUCCESS;
}

void aps6404_enter_qpi(aps6404_dev_struct *dev)
{
    if(APS6404_MODE_SPI == dev->mode) {
        aps6404_simple_cmd(dev, APS6404_CMD_ENTER_QUAD, OSPI_INSTRUCTION_1_LINE);
        dev->mode = APS6404_MODE_QPI;
    }
}

void aps6404_exit_qpi(aps6404_dev_struct *dev)
{
    if(APS6404_MODE_QPI == dev->mode) {
        aps6404_simple_cmd(dev, APS6404_CMD_EXIT_QUAD, OSPI_INSTRUCTION_4_LINES);
        dev->mode = APS6404_MODE_SPI;
    }
}

/*!
    \brief      initialize GPIO, OSPIM, OSPI and the PSRAM
    \param[in]  dev: PSRAM device handle
    \param[in]  ospi_periph: OSPI0 or OSPI1
    \param[in]  ospi_clk_hz: OSPI kernel clock in Hz
    \param[in]  mode: APS6404_MODE_SPI or APS6404_MODE_QPI
    \retval     SUCCESS or ERROR (bad parameter, ID mismatch, SCK too slow for tCEM)
*/
ErrStatus aps6404_init(aps6404_dev_struct *dev, uint32_t ospi_periph, uint32_t ospi_clk_hz, aps6404_mode_enum mode)
{
    aps6404_id_struct id;

    if((NULL == dev) || (0U == ospi_clk_hz) || ((OSPI0 != ospi_periph) && (OSPI1 != ospi_periph))) {
        return ERROR;
    }

    memset(dev, 0, sizeof(aps6404_dev_struct));
    dev->ospi_periph = ospi_periph;
    dev->ospi_clk_hz = ospi_clk_hz;
    dev->mode        = APS6404_MODE_SPI;

    psram_vendor_ospi_deinit(ospi_periph);
    aps6404_gpio_ospim_init(ospi_periph);

    /* Read ID is limited to 33MHz, so reset and ID check run at a low SCK */
    aps6404_ospi_setup(dev, APS6404_SCK_ID_MAX_HZ, DISABLE);

    /* power-up initialization tPU >= 150us */
    delay_1ms(1U);
    aps6404_reset(dev);

    if((ERROR == aps6404_read_id(dev, &id)) || (APS6404_MF_ID != id.mf_id) || (APS6404_KGD_PASS != id.kgd)) {
        return ERROR;
    }

    if(APS6404_MODE_QPI == mode) {
        aps6404_enter_qpi(dev);
    }

    aps6404_ospi_setup(dev, APS6404_SCK_MAX_HZ, ENABLE);
    if(0U == aps6404_chunk_limit(dev)) {
        return ERROR;
    }
    return SUCCESS;
}

static ErrStatus aps6404_check_range(const aps6404_dev_struct *dev, uint32_t addr, uint32_t size)
{
    if((NULL == dev) || (addr > APS6404_SIZE_BYTES) || (size > (APS6404_SIZE_BYTES - addr))) {
        return ERROR;
    }
    return SUCCESS;
}

/* bytes of the next indirect transfer: limited by tCEM and by the 1KB page */
static uint32_t aps6404_next_len(const aps6404_dev_struct *dev, uint32_t addr, uint32_t remain)
{
    uint32_t len = aps6404_chunk_limit(dev);
    uint32_t page_left = APS6404_PAGE_SIZE - (addr & (APS6404_PAGE_SIZE - 1U));

    if(len > page_left) {
        len = page_left;
    }
    return (len > remain) ? remain : len;
}

/*!
    \brief      write data to the PSRAM in indirect mode (quad write 0x38)
*/
ErrStatus aps6404_write(aps6404_dev_struct *dev, uint32_t addr, const uint8_t *pdata, uint32_t size)
{
    psram_vendor_ospi_regular_cmd_struct cmd;
    uint32_t len;

    if((ERROR == aps6404_check_range(dev, addr, size)) || ((0U != size) && (NULL == pdata)) || (0U == aps6404_chunk_limit(dev))) {
        return ERROR;
    }

    while(size > 0U) {
        len = aps6404_next_len(dev, addr, size);

        aps6404_cmd_base(&cmd, OSPI_OPTYPE_COMMON_CFG, aps6404_ins_mode(dev), APS6404_CMD_QUAD_WRITE);
        cmd.addr_mode = OSPI_ADDRESS_4_LINES;
        cmd.address   = addr;
        cmd.data_mode = OSPI_DATA_4_LINES;
        cmd.nbdata    = len;
        psram_vendor_ospi_command_config(dev->ospi_periph, &dev->ospi, &cmd);
        psram_vendor_ospi_transmit(dev->ospi_periph, (uint8_t *)pdata);

        addr  += len;
        pdata += len;
        size  -= len;
    }
    return SUCCESS;
}

/*!
    \brief      read data from the PSRAM in indirect mode (fast read quad 0xEB)
*/
ErrStatus aps6404_read(aps6404_dev_struct *dev, uint32_t addr, uint8_t *pdata, uint32_t size)
{
    psram_vendor_ospi_regular_cmd_struct cmd;
    uint32_t len;

    if((ERROR == aps6404_check_range(dev, addr, size)) || ((0U != size) && (NULL == pdata)) || (0U == aps6404_chunk_limit(dev))) {
        return ERROR;
    }

    while(size > 0U) {
        len = aps6404_next_len(dev, addr, size);

        aps6404_cmd_base(&cmd, OSPI_OPTYPE_COMMON_CFG, aps6404_ins_mode(dev), APS6404_CMD_FAST_READ_QUAD);
        cmd.addr_mode    = OSPI_ADDRESS_4_LINES;
        cmd.address      = addr;
        cmd.dummy_cycles = OSPI_DUMYC_CYCLES_6;
        cmd.data_mode    = OSPI_DATA_4_LINES;
        cmd.nbdata       = len;
        psram_vendor_ospi_command_config(dev->ospi_periph, &dev->ospi, &cmd);
        psram_vendor_ospi_receive(dev->ospi_periph, &dev->ospi, pdata);

        addr  += len;
        pdata += len;
        size  -= len;
    }
    return SUCCESS;
}

/*!
    \brief      enable memory mapped mode, PSRAM is accessible at APS6404_MMAP_BASE(ospi)
    \note       the PSRAM needs CE# released at least every tCEM, the OSPI refresh feature
                and the memory mapped timeout are used for that. Configure the MPU for the
                mapped region (e.g. normal non-cacheable, or write-through) as required.
*/
ErrStatus aps6404_memory_map_enable(aps6404_dev_struct *dev)
{
    psram_vendor_ospi_regular_cmd_struct cmd;
    psram_vendor_ospi_memorymapped_struct mmap_cfg = {0};

    if((NULL == dev) || (0U == aps6404_chunk_limit(dev))) {
        return ERROR;
    }

    /* read: 0xEB, quad address, 6 wait cycles, quad data */
    aps6404_cmd_base(&cmd, OSPI_OPTYPE_READ_CFG, aps6404_ins_mode(dev), APS6404_CMD_FAST_READ_QUAD);
    cmd.addr_mode    = OSPI_ADDRESS_4_LINES;
    cmd.dummy_cycles = OSPI_DUMYC_CYCLES_6;
    cmd.data_mode    = OSPI_DATA_4_LINES;
    psram_vendor_ospi_command_config(dev->ospi_periph, &dev->ospi, &cmd);

    /* write: 0x38, quad address, quad data */
    aps6404_cmd_base(&cmd, OSPI_OPTYPE_WRITE_CFG, aps6404_ins_mode(dev), APS6404_CMD_QUAD_WRITE);
    cmd.addr_mode = OSPI_ADDRESS_4_LINES;
    cmd.data_mode = OSPI_DATA_4_LINES;
    psram_vendor_ospi_command_config(dev->ospi_periph, &dev->ospi, &cmd);

    while(RESET != psram_vendor_ospi_flag_get(dev->ospi_periph, OSPI_FLAG_BUSY)) {
    }

    /* enabled here instead of in psram_vendor_ospi_memorymapped_mode() which overwrites OSPI_CTL when the timeout is requested */
    psram_vendor_ospi_timeout_cycle_config(dev->ospi_periph, APS6404_MMAP_TIMEOUT);
    psram_vendor_ospi_timeout_enable(dev->ospi_periph);

    mmap_cfg.timeout_activation = OSPI_TIMEOUT_COUNT_DISABLE;
    mmap_cfg.timeout_period     = 0U;
    psram_vendor_ospi_memorymapped_mode(dev->ospi_periph, &mmap_cfg);
    if(OSPI0 == dev->ospi_periph) {
        NVIC_SetPriority(OSPI0_IRQn, 0U);
        NVIC_ClearPendingIRQ(OSPI0_IRQn);
        NVIC_EnableIRQ(OSPI0_IRQn);
    }
    psram_vendor_ospi_interrupt_enable(dev->ospi_periph, OSPI_INT_TMOUT);
    __DSB();
    __ISB();
    return SUCCESS;
}

/*!
    \brief      leave memory mapped mode, indirect access is available again
*/
void aps6404_memory_map_disable(aps6404_dev_struct *dev)
{
    /* let the posted memory mapped writes reach the OSPI before the mode is left */
    __DSB();
    psram_vendor_ospi_interrupt_disable(dev->ospi_periph, OSPI_INT_TMOUT);
    if(OSPI0 == dev->ospi_periph) {
        NVIC_DisableIRQ(OSPI0_IRQn);
        NVIC_ClearPendingIRQ(OSPI0_IRQn);
    }
    psram_vendor_ospi_transmission_abort(dev->ospi_periph);
    psram_vendor_ospi_timeout_disable(dev->ospi_periph);
    /* psram_vendor_ospi_transmission_abort() only aborts a busy transfer, force the indirect mode */
    OSPI_CTL(dev->ospi_periph) &= ~OSPI_CTL_FMOD;
}

/*!
    \brief      indirect read/write test, includes a transfer crossing the 1KB page boundary
    \retval     SUCCESS if the data read back matches
*/
ErrStatus aps6404_selftest(aps6404_dev_struct *dev)
{
    static const uint32_t test_addr[] = {0x000000U, 0x0003F0U, 0x123456U, APS6404_SIZE_BYTES - 64U};
    uint8_t tx[64];
    uint8_t rx[64];
    uint32_t i;
    uint32_t j;

    for(i = 0U; i < (sizeof(test_addr) / sizeof(test_addr[0])); i++) {
        for(j = 0U; j < sizeof(tx); j++) {
            tx[j] = (uint8_t)(j * 7U + i * 31U + 0x5AU);
        }
        memset(rx, 0, sizeof(rx));
        if((ERROR == aps6404_write(dev, test_addr[i], tx, sizeof(tx))) ||
           (ERROR == aps6404_read(dev, test_addr[i], rx, sizeof(rx))) ||
           (0 != memcmp(tx, rx, sizeof(tx)))) {
            return ERROR;
        }
    }
    return SUCCESS;
}
