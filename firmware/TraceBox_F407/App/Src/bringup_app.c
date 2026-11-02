#include "bringup_app.h"

#include <stdbool.h>
#include <stdint.h>

#include "main.h"
#include "core_selftest.h"
#include "tracebox_adc_dma.h"
#include "tracebox_board.h"
#include "tracebox_nor.h"
#include "tracebox_page_codec.h"
#include "tracebox_recorder.h"
#include "tracebox_spi.h"
#include "tracebox_spi_ingress.h"
#include "tracebox_uart.h"

#define TB_LINE_CAPACITY       64U
#define TB_HEARTBEAT_PERIOD_MS 500U
#define TB_UID_BASE_ADDRESS    0x1FFF7A10UL
#define TB_FLASH_SIZE_ADDRESS  0x1FFF7A22UL
#define TB_SPI_TIMEOUT_CYCLES  16800000UL
#define TB_SPI_FAULT_TIMEOUT_CYCLES 16800UL
#define TB_ADC_TEST_BLOCK_COUNT 200U
#define TB_ADC_EXPECTED_BLOCK_CYCLES 1612800UL
#define TB_ADC_BLOCK_TOLERANCE_CYCLES 32256UL
#define TB_FINAL_CAPTURE_ID   0x20260914UL
#define TB_TRACE_CAPTURE_ID   0x54523201UL
#define TB_CONTROL_BAUDRATE   115200UL
#define TB_DEFAULT_DUMP_BAUD  460800UL
#define TB_DUMP_SWITCH_DELAY_MS 100U

static uint8_t s_spi_tx[TB_SPI_MAX_TRANSFER_LENGTH];
static uint8_t s_spi_rx[TB_SPI_MAX_TRANSFER_LENGTH];
static const uint16_t s_spi_test_lengths[] = {16U, 256U, 4096U};
static uint8_t s_nor_page[TB_NOR_PAGE_SIZE_BYTES];
static uint8_t s_nor_readback[TB_NOR_PAGE_SIZE_BYTES];
static uint16_t s_nor_samples[TB_PAGE_SAMPLE_COUNT];

typedef enum
{
    APP_NOR_TEST_IDLE = 0,
    APP_NOR_TEST_IDENTIFY,
    APP_NOR_TEST_ERASE,
    APP_NOR_TEST_ERASED_READ,
    APP_NOR_TEST_PROGRAM_BODY,
    APP_NOR_TEST_PROGRAM_COMMIT,
    APP_NOR_TEST_VERIFY_READ,
    APP_NOR_TEST_FORCED_TIMEOUT,
    APP_NOR_TEST_RECOVER
} AppNorTestStage;

typedef enum
{
    APP_DUMP_IDLE = 0,
    APP_DUMP_ANNOUNCE,
    APP_DUMP_SWITCH_DELAY,
    APP_DUMP_READ_START,
    APP_DUMP_READ_WAIT,
    APP_DUMP_SEND_PAGE,
    APP_DUMP_END,
    APP_DUMP_RESTORE_DELAY,
    APP_DUMP_RESULT,
    APP_DUMP_ERROR
} AppDumpStage;

typedef struct
{
    uint32_t reset_flags;
    uint32_t dwt_cycles_10ms;
    uint32_t line_overflow_count;
    uint32_t command_count;
    uint32_t unknown_command_count;
    uint32_t last_heartbeat_ms;
    uint8_t line_length;
    bool line_discard;
    bool previous_was_cr;
    bool heartbeat_enabled;
    bool spi_dma_active;
    bool spi_dma_suite_active;
    bool spi_dma_suite_passed;
    uint8_t spi_dma_next_test;
    uint32_t spi_dma_length;
    uint32_t spi_dma_start_cycles;
    uint32_t spi_dma_foreground_iterations;
    bool adc_test_active;
    uint32_t adc_target_blocks;
    uint32_t adc_blocks_checked;
    uint32_t adc_sequence_errors;
    uint32_t adc_buffer_errors;
    uint32_t adc_samples_checked;
    uint32_t adc_sample_sum;
    uint32_t adc_previous_completed_cycles;
    uint32_t adc_period_count;
    uint32_t adc_period_sum_cycles;
    uint32_t adc_period_min_cycles;
    uint32_t adc_period_max_cycles;
    uint32_t adc_period_errors;
    uint16_t adc_sample_min;
    uint16_t adc_sample_max;
    bool nor_test_active;
    AppNorTestStage nor_test_stage;
    uint32_t nor_scratch_address;
    uint32_t nor_page_crc;
    uint32_t nor_calculated_crc;
    uint32_t nor_erased_not_ff;
    uint32_t nor_mismatch_count;
    uint32_t nor_timeout_observed;
    bool boot_recovery_active;
    bool torn_fault_active;
    uint32_t torn_fault_page;
    AppDumpStage dump_stage;
    uint32_t dump_page_count;
    uint32_t dump_page_index;
    uint32_t dump_baudrate;
    uint32_t dump_deadline_ms;
    uint32_t dump_bytes_sent;
    TbStatus dump_status;
    uint8_t dump_page[TB_NOR_PAGE_SIZE_BYTES];
    char line[TB_LINE_CAPACITY];
} BringupContext;

static BringupContext s_app;

static bool App_StartSpiDma(uint32_t length);
static bool App_StartSpiDmaWithTimeout(uint32_t length, uint32_t timeout_cycles);
static void App_SpiDmaComplete(TbStatus status, void *user_context);

static void App_Write(const char *text)
{
    (void)TbUart_WriteString(text);
}

static void App_WriteU32(uint32_t value)
{
    char buffer[11];
    uint8_t length = 0U;
    uint8_t index;

    do
    {
        buffer[length++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (length < sizeof(buffer)));

    for (index = 0U; index < (length / 2U); ++index)
    {
        char temp = buffer[index];
        buffer[index] = buffer[length - 1U - index];
        buffer[length - 1U - index] = temp;
    }

    (void)TbUart_Write((const uint8_t *)buffer, length);
}

static void App_WriteHex32(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char buffer[10];
    uint8_t index;

    buffer[0] = '0';
    buffer[1] = 'x';
    for (index = 0U; index < 8U; ++index)
    {
        uint8_t shift = (uint8_t)((7U - index) * 4U);
        buffer[index + 2U] = digits[(value >> shift) & 0x0FU];
    }
    (void)TbUart_Write((const uint8_t *)buffer, sizeof(buffer));
}

static void App_WriteHex8(uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char buffer[4];

    buffer[0] = '0';
    buffer[1] = 'x';
    buffer[2] = digits[(value >> 4U) & 0x0FU];
    buffer[3] = digits[value & 0x0FU];
    (void)TbUart_Write((const uint8_t *)buffer, sizeof(buffer));
}

static bool App_StringEquals(const char *left, const char *right)
{
    while ((*left != '\0') && (*right != '\0') && (*left == *right))
    {
        ++left;
        ++right;
    }
    return (*left == '\0') && (*right == '\0');
}

static bool App_TimeReached(uint32_t now_ms, uint32_t deadline_ms)
{
    return ((int32_t)(now_ms - deadline_ms) >= 0);
}

static bool App_ParseSeconds(const char *command,
                             const char *prefix,
                             uint32_t *seconds_out)
{
    uint32_t value = 0U;

    if ((command == 0) || (prefix == 0) || (seconds_out == 0))
    {
        return false;
    }
    while ((*prefix != '\0') && (*command == *prefix))
    {
        ++command;
        ++prefix;
    }
    if ((*prefix != '\0') || (*command != ' '))
    {
        return false;
    }
    ++command;
    if ((*command < '0') || (*command > '9'))
    {
        return false;
    }
    while ((*command >= '0') && (*command <= '9'))
    {
        uint32_t digit = (uint32_t)(*command - '0');

        if (value > 1000U)
        {
            return false;
        }
        value = (value * 10U) + digit;
        ++command;
    }
    while (*command == ' ')
    {
        ++command;
    }
    if ((*command != '\0') || (value == 0U) || (value > 60U))
    {
        return false;
    }
    *seconds_out = value;
    return true;
}

static bool App_ParseCount(const char *command,
                           const char *prefix,
                           uint32_t maximum,
                           uint32_t *count_out)
{
    uint32_t value = 0U;

    if ((command == 0) || (prefix == 0) || (count_out == 0) ||
        (maximum == 0U))
    {
        return false;
    }
    while ((*prefix != '\0') && (*command == *prefix))
    {
        ++command;
        ++prefix;
    }
    if ((*prefix != '\0') || (*command != ' '))
    {
        return false;
    }
    ++command;
    if ((*command < '0') || (*command > '9'))
    {
        return false;
    }
    while ((*command >= '0') && (*command <= '9'))
    {
        uint32_t digit = (uint32_t)(*command - '0');

        if (value > ((maximum - digit) / 10U))
        {
            return false;
        }
        value = (value * 10U) + digit;
        ++command;
    }
    while (*command == ' ')
    {
        ++command;
    }
    if ((*command != '\0') || (value == 0U) || (value > maximum))
    {
        return false;
    }
    *count_out = value;
    return true;
}

static uint32_t App_PagesForSeconds(uint32_t seconds)
{
    return (seconds * 10000UL) / TB_PAGE_SAMPLE_COUNT;
}

static uint32_t App_MeasureDwt10ms(void)
{
    uint32_t tick;
    uint32_t start;

    if (!TbBoard_DwtAvailable())
    {
        return 0U;
    }

    tick = HAL_GetTick();
    while (HAL_GetTick() == tick)
    {
        /* Align to a millisecond boundary. */
    }

    tick = HAL_GetTick();
    start = TbBoard_DwtCycles();
    while ((uint32_t)(HAL_GetTick() - tick) < 10U)
    {
        /* Interrupts stay enabled; UART RX still only publishes bytes. */
    }

    return TbBoard_DwtCycles() - start;
}

static void App_WriteResetFlags(uint32_t flags)
{
    bool any = false;

#define APP_RESET_FLAG(mask, name) \
    do { if ((flags & (mask)) != 0U) { if (any) { App_Write("|"); } App_Write(name); any = true; } } while (0)

    APP_RESET_FLAG(RCC_CSR_BORRSTF, "BOR");
    APP_RESET_FLAG(RCC_CSR_PINRSTF, "PIN");
    APP_RESET_FLAG(RCC_CSR_PORRSTF, "POR");
    APP_RESET_FLAG(RCC_CSR_SFTRSTF, "SOFT");
    APP_RESET_FLAG(RCC_CSR_IWDGRSTF, "IWDG");
    APP_RESET_FLAG(RCC_CSR_WWDGRSTF, "WWDG");
    APP_RESET_FLAG(RCC_CSR_LPWRRSTF, "LOW_POWER");

#undef APP_RESET_FLAG

    if (!any)
    {
        App_Write("UNKNOWN");
    }
}

static void App_PrintPins(void)
{
    App_Write("pins:\r\n");
    App_Write("  PC0  LED active-low (configured)\r\n");
    App_Write("  PA9  USART1_TX AF7 (configured)\r\n");
    App_Write("  PA10 USART1_RX AF7 (configured)\r\n");
    App_Write("  PA0  ADC1_IN0 (analog, TIM2 10kHz + DMA2 S4/C0)\r\n");
    App_Write("  PA5/PA6/PA7 SPI1 SCK/MISO/MOSI (AF5, configured)\r\n");
    App_Write("  PB0  W25Q64 CS# (output, idle high)\r\n");
    App_Write("  PB12/PB13/PB15 SPI2 NSS/SCK/MOSI (slave RX DMA)\r\n");
    App_Write("  PB14 SPI2 MISO (reserved; V2 RX-only)\r\n");
    App_Write("  PE0  TRACE_READY output (high=accept one frame)\r\n");
    App_Write("  PA13/PA14 SWDIO/SWCLK (do not connect peripherals)\r\n");
}

static void App_PrintInfo(void)
{
    const volatile uint32_t *uid = (const volatile uint32_t *)TB_UID_BASE_ADDRESS;
    uint16_t flash_kib = *(const volatile uint16_t *)TB_FLASH_SIZE_ADDRESS;

    App_Write("board=LXB407VE-P1\r\n");
    App_Write("firmware=TraceBox_RT_v2.0.0\r\n");
    App_Write("cpuid=");
    App_WriteHex32(SCB->CPUID);
    App_Write("\r\ndbgmcu_idcode=");
    App_WriteHex32(DBGMCU->IDCODE);
    App_Write("\r\nflash_kib=");
    App_WriteU32(flash_kib);
    App_Write("\r\nuid=");
    App_WriteHex32(uid[2]);
    App_Write("-");
    App_WriteHex32(uid[1]);
    App_Write("-");
    App_WriteHex32(uid[0]);
    App_Write("\r\nreset=");
    App_WriteResetFlags(s_app.reset_flags);
    App_Write("\r\nsysclk_hz=");
    App_WriteU32(HAL_RCC_GetSysClockFreq());
    App_Write("\r\nhclk_hz=");
    App_WriteU32(HAL_RCC_GetHCLKFreq());
    App_Write("\r\npclk1_hz=");
    App_WriteU32(HAL_RCC_GetPCLK1Freq());
    App_Write("\r\npclk2_hz=");
    App_WriteU32(HAL_RCC_GetPCLK2Freq());
    App_Write("\r\ndwt_10ms_cycles=");
    App_WriteU32(s_app.dwt_cycles_10ms);
    App_Write(" (expected about 1680000)\r\n");
}

static void App_PrintStats(void)
{
    TbAdcDmaStats adc_stats;
    TbNorStats nor_stats;
    TbRecorderStats recorder_stats;
    TbSpiStats spi_stats;
    TbSpiIngressStats ingress_stats;
    TbUartStats stats;

    TbUart_GetStats(&stats);
    App_Write("uart_irq=");
    App_WriteU32(stats.irq_count);
    App_Write(" rx_bytes=");
    App_WriteU32(stats.rx_bytes);
    App_Write(" tx_bytes=");
    App_WriteU32(stats.tx_bytes);
    App_Write("\r\nrx_pending=");
    App_WriteU32(stats.rx_pending);
    App_Write(" rx_high_water=");
    App_WriteU32(stats.rx_high_water);
    App_Write(" rx_overflow=");
    App_WriteU32(stats.rx_overflow_count);
    App_Write(" uart_errors=");
    App_WriteU32(stats.peripheral_error_count);
    App_Write(" baud=");
    App_WriteU32(stats.baudrate);
    App_Write(" baud_switches=");
    App_WriteU32(stats.baud_switch_count);
    App_Write(" tx_timeouts=");
    App_WriteU32(stats.tx_timeout_count);
    App_Write("\r\ncommands=");
    App_WriteU32(s_app.command_count);
    App_Write(" unknown=");
    App_WriteU32(s_app.unknown_command_count);
    App_Write(" line_overflow=");
    App_WriteU32(s_app.line_overflow_count);
    App_Write("\r\n");

    TbSpiIngress_GetStats(&ingress_stats);
    App_Write("ingress_state=");
    App_WriteU32((uint32_t)ingress_stats.state);
    App_Write(" ready=");
    App_Write(ingress_stats.ready_asserted ? "1" : "0");
    App_Write(" produced=");
    App_WriteU32(ingress_stats.frames_produced);
    App_Write(" consumed=");
    App_WriteU32(ingress_stats.frames_consumed);
    App_Write(" pending=");
    App_WriteU32(ingress_stats.descriptor_pending);
    App_Write(" hwm=");
    App_WriteU32(ingress_stats.descriptor_high_water);
    App_Write(" overflow=");
    App_WriteU32(ingress_stats.descriptor_overflow_count);
    App_Write("\r\ningress_dma_errors=");
    App_WriteU32(ingress_stats.dma_error_count);
    App_Write(" spi_errors=");
    App_WriteU32(ingress_stats.spi_error_count);
    App_Write(" short_frames=");
    App_WriteU32(ingress_stats.short_frame_count);
    App_Write(" recoveries=");
    App_WriteU32(ingress_stats.recovery_count);
    App_Write(" isr_max_cycles=");
    App_WriteU32(ingress_stats.max_isr_cycles);
    App_Write("\r\n");

    TbSpi_GetStats(&spi_stats);
    App_Write("spi_poll_transactions=");
    App_WriteU32(spi_stats.polling_transaction_count);
    App_Write(" spi_poll_bytes=");
    App_WriteU32(spi_stats.polling_transferred_bytes);
    App_Write(" spi_dma_transactions=");
    App_WriteU32(spi_stats.dma_transaction_count);
    App_Write(" spi_dma_bytes=");
    App_WriteU32(spi_stats.dma_transferred_bytes);
    App_Write(" spi_dma_irqs=");
    App_WriteU32(spi_stats.dma_irq_count);
    App_Write(" spi_dma_errors=");
    App_WriteU32(spi_stats.dma_error_count);
    App_Write("\r\n");
    App_Write("spi_timeouts=");
    App_WriteU32(spi_stats.timeout_count);
    App_Write(" spi_hw_errors=");
    App_WriteU32(spi_stats.hardware_error_count);
    App_Write(" spi_recoveries=");
    App_WriteU32(spi_stats.recovery_count);
    App_Write("\r\n");

    TbAdcDma_GetStats(&adc_stats);
    App_Write("adc_state=");
    App_WriteU32((uint32_t)adc_stats.state);
    App_Write(" adc_blocks_produced=");
    App_WriteU32(adc_stats.blocks_produced);
    App_Write(" adc_blocks_consumed=");
    App_WriteU32(adc_stats.blocks_consumed);
    App_Write(" adc_dma_irqs=");
    App_WriteU32(adc_stats.dma_irq_count);
    App_Write("\r\nadc_descriptor_pending=");
    App_WriteU32(adc_stats.descriptor_pending);
    App_Write(" adc_descriptor_hwm=");
    App_WriteU32(adc_stats.descriptor_high_water);
    App_Write(" adc_descriptor_overflow=");
    App_WriteU32(adc_stats.descriptor_overflow_count);
    App_Write(" adc_dma_errors=");
    App_WriteU32(adc_stats.dma_error_count);
    App_Write(" adc_overruns=");
    App_WriteU32(adc_stats.adc_overrun_count);
    App_Write(" adc_isr_max_cycles=");
    App_WriteU32(adc_stats.max_isr_cycles);
    App_Write("\r\n");

    TbNor_GetStats(&nor_stats);
    App_Write("nor_state=");
    App_WriteU32((uint32_t)nor_stats.state);
    App_Write(" nor_identify=");
    App_WriteU32(nor_stats.identify_count);
    App_Write(" nor_erases=");
    App_WriteU32(nor_stats.sector_erase_count);
    App_Write(" nor_programs=");
    App_WriteU32(nor_stats.page_program_count);
    App_Write(" nor_reads=");
    App_WriteU32(nor_stats.read_count);
    App_Write(" nor_polls=");
    App_WriteU32(nor_stats.status_poll_count);
    App_Write("\r\nnor_timeouts=");
    App_WriteU32(nor_stats.timeout_count);
    App_Write(" nor_spi_errors=");
    App_WriteU32(nor_stats.spi_error_count);
    App_Write(" nor_wel_errors=");
    App_WriteU32(nor_stats.write_enable_error_count);
    App_Write(" nor_recoveries=");
    App_WriteU32(nor_stats.recovery_count);
    App_Write("\r\n");

    TbRecorder_GetStats(&recorder_stats);
    App_Write("recorder_state=");
    App_WriteU32((uint32_t)recorder_stats.state);
    App_Write(" recorder_packed=");
    App_WriteU32(recorder_stats.pages_packed);
    App_Write(" recorder_committed=");
    App_WriteU32(recorder_stats.pages_committed);
    App_Write(" recorder_verified=");
    App_WriteU32(recorder_stats.pages_verified);
    App_Write(" recorder_pool_hwm=");
    App_WriteU32(recorder_stats.page_pool_high_water);
    App_Write(" recorder_errors=");
    App_WriteU32(recorder_stats.flash_errors + recorder_stats.page_crc_errors +
                 recorder_stats.sequence_errors);
    App_Write("\r\n");
}

static const char *App_StatusText(TbStatus status)
{
    switch (status)
    {
        case TB_STATUS_OK:
            return "OK";
        case TB_STATUS_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case TB_STATUS_TIMEOUT:
            return "TIMEOUT";
        case TB_STATUS_OVERFLOW:
            return "OVERFLOW";
        case TB_STATUS_HW_ERROR:
            return "HW_ERROR";
        case TB_STATUS_BUSY:
            return "BUSY";
        case TB_STATUS_VERIFY_FAILED:
            return "VERIFY_FAILED";
        case TB_STATUS_CRC_ERROR:
            return "CRC_ERROR";
        case TB_STATUS_STATE_ERROR:
            return "STATE_ERROR";
        case TB_STATUS_RANGE_ERROR:
            return "RANGE_ERROR";
        case TB_STATUS_FORMAT_ERROR:
            return "FORMAT_ERROR";
        default:
            return "UNKNOWN_STATUS";
    }
}

static void App_RunCoreSelfTest(void)
{
    TbCoreSelfTestResult result;
    TbStatus status = CoreSelfTest_Run(&result);

    App_Write("core_status=");
    App_Write(App_StatusText(status));
    App_Write(" failure_step=");
    App_WriteU32(result.failure_step);
    App_Write("\r\nevent_pushed=");
    App_WriteU32(result.event_stats.pushed);
    App_Write(" event_popped=");
    App_WriteU32(result.event_stats.popped);
    App_Write(" event_high_water=");
    App_WriteU32(result.event_stats.high_water);
    App_Write(" event_dropped_newest=");
    App_WriteU32(result.event_stats.dropped_newest);
    App_Write("\r\npool_in_use_high_water=");
    App_WriteU32(result.pool_stats.in_use_high_water);
    App_Write(" pool_ready_high_water=");
    App_WriteU32(result.pool_stats.ready_high_water);
    App_Write(" pool_overflow=");
    App_WriteU32(result.pool_stats.overflow_count);
    App_Write(" pool_ownership_errors=");
    App_WriteU32(result.pool_stats.ownership_error_count);
    App_Write("\r\npage_crc=");
    App_WriteHex32(result.page_crc);
    App_Write(" crc_corruption_rejected=");
    App_WriteU32(result.crc_corruption_rejected);
    App_Write(" restored_page_valid=");
    App_WriteU32(result.post_corruption_page_valid);
    App_Write("\r\n");
    App_Write((status == TB_STATUS_OK) ?
              "CORE SELFTEST: PASS\r\n" : "CORE SELFTEST: FAIL\r\n");
}

static void App_PrintSpiInfo(void)
{
    TbSpiStats stats;

    TbSpi_GetStats(&stats);
    App_Write("spi1_mode=master,mode0,8bit,msb-first\r\n");
    App_Write("spi1_pclk_hz=");
    App_WriteU32(stats.peripheral_clock_hz);
    App_Write(" prescaler=");
    App_WriteU32(stats.prescaler);
    App_Write(" sck_hz=");
    App_WriteU32(stats.sck_hz);
    App_Write("\r\nloopback_wire=PA7(MOSI)->PA6(MISO)\r\n");
}

static uint32_t App_PrbsNext(uint32_t state)
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

static void App_PrepareSpiBuffers(uint32_t length)
{
    uint32_t index;
    uint32_t prbs_state = 0x4F1BBCDCUL ^ length;

    for (index = 0U; index < length; ++index)
    {
        prbs_state = App_PrbsNext(prbs_state);
        s_spi_tx[index] = (uint8_t)prbs_state;
        s_spi_rx[index] = 0xA5U;
    }
}

static uint32_t App_FindSpiMismatch(uint32_t length)
{
    uint32_t index;

    for (index = 0U; index < length; ++index)
    {
        if (s_spi_rx[index] != s_spi_tx[index])
        {
            return index;
        }
    }

    return length;
}

static bool App_RunSpiLoopback(uint32_t length)
{
    TbSpiTransfer transfer;
    TbSpiStats stats;
    TbStatus status;
    uint32_t mismatch_index = length;
    uint32_t bytes_per_second = 0U;

    TbSpi_GetStats(&stats);
    if (stats.state != TB_SPI_STATE_READY)
    {
        App_Write("SPI POLLING status=BUSY\r\n");
        return false;
    }

    App_PrepareSpiBuffers(length);

    transfer.tx_data = s_spi_tx;
    transfer.rx_data = s_spi_rx;
    transfer.length = length;
    transfer.timeout_cycles = TB_SPI_TIMEOUT_CYCLES;
    status = TbSpi_TransferPolling(&transfer);
    TbSpi_GetStats(&stats);

    if (status == TB_STATUS_OK)
    {
        mismatch_index = App_FindSpiMismatch(length);
        if (mismatch_index != length)
        {
            status = TB_STATUS_VERIFY_FAILED;
        }
    }

    if (((status == TB_STATUS_OK) || (status == TB_STATUS_VERIFY_FAILED)) &&
        (stats.last_transfer_cycles != 0U))
    {
        bytes_per_second = (uint32_t)
            (((uint64_t)length * (uint64_t)HAL_RCC_GetHCLKFreq()) /
             stats.last_transfer_cycles);
    }

    App_Write("SPI POLLING length=");
    App_WriteU32(length);
    App_Write(" status=");
    App_Write(App_StatusText(status));
    App_Write(" cycles=");
    App_WriteU32(stats.last_transfer_cycles);
    App_Write(" bytes_per_s=");
    App_WriteU32(bytes_per_second);

    if (status == TB_STATUS_VERIFY_FAILED)
    {
        App_Write(" mismatch_index=");
        App_WriteU32(mismatch_index);
        App_Write(" expected=");
        App_WriteHex8(s_spi_tx[mismatch_index]);
        App_Write(" received=");
        App_WriteHex8(s_spi_rx[mismatch_index]);
    }

    App_Write("\r\n");
    return status == TB_STATUS_OK;
}

static bool App_StartSpiDmaWithTimeout(uint32_t length, uint32_t timeout_cycles)
{
    TbSpiDmaTransfer transfer;
    TbSpiStats stats;
    TbStatus status;
    uint32_t started_at;

    TbSpi_GetStats(&stats);
    if ((stats.state != TB_SPI_STATE_READY) || s_app.spi_dma_active)
    {
        App_Write("SPI DMA start status=BUSY\r\n");
        return false;
    }

    App_PrepareSpiBuffers(length);
    transfer.tx_data = s_spi_tx;
    transfer.rx_data = s_spi_rx;
    transfer.length = length;
    transfer.timeout_cycles = timeout_cycles;
    transfer.completion_callback = App_SpiDmaComplete;
    transfer.user_context = 0;

    s_app.spi_dma_length = length;
    s_app.spi_dma_foreground_iterations = 0U;
    started_at = TbBoard_DwtCycles();
    status = TbSpi_TransferDmaStart(&transfer);
    s_app.spi_dma_start_cycles = TbBoard_DwtCycles() - started_at;

    if (status != TB_STATUS_OK)
    {
        App_Write("SPI DMA start status=");
        App_Write(App_StatusText(status));
        App_Write("\r\n");
        return false;
    }

    s_app.spi_dma_active = true;
    App_Write("SPI DMA started length=");
    App_WriteU32(length);
    App_Write(" start_api_cycles=");
    App_WriteU32(s_app.spi_dma_start_cycles);
    App_Write("\r\n");
    return true;
}

static bool App_StartSpiDma(uint32_t length)
{
    return App_StartSpiDmaWithTimeout(length, TB_SPI_TIMEOUT_CYCLES);
}

static void App_SpiDmaComplete(TbStatus status, void *user_context)
{
    TbSpiStats stats;
    uint32_t mismatch_index = s_app.spi_dma_length;
    uint32_t bytes_per_second = 0U;
    bool passed;

    (void)user_context;
    s_app.spi_dma_active = false;
    TbSpi_GetStats(&stats);

    if (status == TB_STATUS_OK)
    {
        mismatch_index = App_FindSpiMismatch(s_app.spi_dma_length);
        if (mismatch_index != s_app.spi_dma_length)
        {
            status = TB_STATUS_VERIFY_FAILED;
        }
    }

    if (((status == TB_STATUS_OK) || (status == TB_STATUS_VERIFY_FAILED)) &&
        (stats.dma_last_transfer_cycles != 0U))
    {
        bytes_per_second = (uint32_t)
            (((uint64_t)s_app.spi_dma_length * (uint64_t)HAL_RCC_GetHCLKFreq()) /
             stats.dma_last_transfer_cycles);
    }

    App_Write("SPI DMA length=");
    App_WriteU32(s_app.spi_dma_length);
    App_Write(" status=");
    App_Write(App_StatusText(status));
    App_Write(" transfer_cycles=");
    App_WriteU32(stats.dma_last_transfer_cycles);
    App_Write(" start_api_cycles=");
    App_WriteU32(s_app.spi_dma_start_cycles);
    App_Write(" bytes_per_s=");
    App_WriteU32(bytes_per_second);
    App_Write(" foreground_iterations=");
    App_WriteU32(s_app.spi_dma_foreground_iterations);

    if (status == TB_STATUS_VERIFY_FAILED)
    {
        App_Write(" mismatch_index=");
        App_WriteU32(mismatch_index);
        App_Write(" expected=");
        App_WriteHex8(s_spi_tx[mismatch_index]);
        App_Write(" received=");
        App_WriteHex8(s_spi_rx[mismatch_index]);
    }
    App_Write("\r\n");

    passed = (status == TB_STATUS_OK);
    if (s_app.spi_dma_suite_active)
    {
        s_app.spi_dma_suite_passed = s_app.spi_dma_suite_passed && passed;
        if (s_app.spi_dma_next_test <
            (sizeof(s_spi_test_lengths) / sizeof(s_spi_test_lengths[0])))
        {
            uint32_t next_length = s_spi_test_lengths[s_app.spi_dma_next_test++];
            if (!App_StartSpiDma(next_length))
            {
                s_app.spi_dma_suite_passed = false;
                s_app.spi_dma_suite_active = false;
                App_Write("SPI DMA LOOPBACK SUITE: FAIL start\r\n");
            }
        }
        else
        {
            s_app.spi_dma_suite_active = false;
            App_Write(s_app.spi_dma_suite_passed ?
                      "SPI DMA LOOPBACK SUITE: PASS\r\n" :
                      "SPI DMA LOOPBACK SUITE: FAIL\r\n");
        }
    }
}

static void App_PrintAdcInfo(void)
{
    TbAdcDmaStats stats;

    TbAdcDma_GetStats(&stats);
    App_Write("adc1_channel=0 pin=PA0 resolution_bits=12\r\n");
    App_Write("adc_trigger=TIM2_TRGO sample_rate_hz=");
    App_WriteU32(stats.sample_rate_hz);
    App_Write(" adc_clock_hz=");
    App_WriteU32(stats.adc_clock_hz);
    App_Write("\r\nadc_dma=DMA2_Stream4_Channel0 mode=circular_double_buffer\r\n");
    App_Write("adc_buffers=2 samples_per_buffer=");
    App_WriteU32(TB_ADC_DMA_SAMPLES_PER_BUFFER);
    App_Write(" descriptor_capacity=");
    App_WriteU32(TB_ADC_DMA_DESCRIPTOR_CAPACITY);
    App_Write(" block_period_us=9600\r\n");
}

static void App_FinishAdcTest(TbStatus status)
{
    TbAdcDmaStats stats;
    uint32_t average = 0U;
    uint32_t period_average = 0U;
    bool passed;

    if (status == TB_STATUS_OK)
    {
        status = TbAdcDma_Stop();
    }
    else
    {
        (void)TbAdcDma_Stop();
    }

    s_app.adc_test_active = false;
    TbAdcDma_GetStats(&stats);
    if (s_app.adc_samples_checked != 0U)
    {
        average = s_app.adc_sample_sum / s_app.adc_samples_checked;
    }
    if (s_app.adc_period_count != 0U)
    {
        period_average =
            s_app.adc_period_sum_cycles / s_app.adc_period_count;
    }

    passed = (status == TB_STATUS_OK) &&
             (s_app.adc_blocks_checked == s_app.adc_target_blocks) &&
             (s_app.adc_samples_checked ==
              (s_app.adc_target_blocks * TB_ADC_DMA_SAMPLES_PER_BUFFER)) &&
             (s_app.adc_sequence_errors == 0U) &&
             (s_app.adc_buffer_errors == 0U) &&
             (s_app.adc_period_count == (s_app.adc_target_blocks - 1U)) &&
             (s_app.adc_period_errors == 0U) &&
             (stats.descriptor_overflow_count == 0U) &&
             (stats.dma_error_count == 0U) &&
             (stats.adc_overrun_count == 0U);

    App_Write("ADC DMA result status=");
    App_Write(App_StatusText(status));
    App_Write(" blocks=");
    App_WriteU32(s_app.adc_blocks_checked);
    App_Write(" samples=");
    App_WriteU32(s_app.adc_samples_checked);
    App_Write(" sample_rate_hz=");
    App_WriteU32(stats.sample_rate_hz);
    App_Write(" samples_per_buffer=");
    App_WriteU32(TB_ADC_DMA_SAMPLES_PER_BUFFER);
    App_Write(" min=");
    App_WriteU32(s_app.adc_sample_min);
    App_Write(" max=");
    App_WriteU32(s_app.adc_sample_max);
    App_Write(" avg=");
    App_WriteU32(average);
    App_Write("\r\nseq_errors=");
    App_WriteU32(s_app.adc_sequence_errors);
    App_Write(" buffer_errors=");
    App_WriteU32(s_app.adc_buffer_errors);
    App_Write(" produced=");
    App_WriteU32(stats.blocks_produced);
    App_Write(" consumed=");
    App_WriteU32(stats.blocks_consumed);
    App_Write(" descriptor_hwm=");
    App_WriteU32(stats.descriptor_high_water);
    App_Write(" descriptor_overflow=");
    App_WriteU32(stats.descriptor_overflow_count);
    App_Write("\r\nperiod_count=");
    App_WriteU32(s_app.adc_period_count);
    App_Write(" period_min_cycles=");
    App_WriteU32(s_app.adc_period_min_cycles);
    App_Write(" period_max_cycles=");
    App_WriteU32(s_app.adc_period_max_cycles);
    App_Write(" period_avg_cycles=");
    App_WriteU32(period_average);
    App_Write(" period_errors=");
    App_WriteU32(s_app.adc_period_errors);
    App_Write("\r\ndma_irqs=");
    App_WriteU32(stats.dma_irq_count);
    App_Write(" dma_errors=");
    App_WriteU32(stats.dma_error_count);
    App_Write(" adc_overruns=");
    App_WriteU32(stats.adc_overrun_count);
    App_Write(" isr_max_cycles=");
    App_WriteU32(stats.max_isr_cycles);
    App_Write("\r\n");
    App_Write(passed ? "ADC DMA TEST: PASS\r\n" : "ADC DMA TEST: FAIL\r\n");
}

static void App_StartAdcTest(void)
{
    TbStatus status;

    if (s_app.adc_test_active)
    {
        App_Write("ADC DMA start status=BUSY\r\n");
        return;
    }

    s_app.adc_target_blocks = TB_ADC_TEST_BLOCK_COUNT;
    s_app.adc_blocks_checked = 0U;
    s_app.adc_sequence_errors = 0U;
    s_app.adc_buffer_errors = 0U;
    s_app.adc_samples_checked = 0U;
    s_app.adc_sample_sum = 0U;
    s_app.adc_previous_completed_cycles = 0U;
    s_app.adc_period_count = 0U;
    s_app.adc_period_sum_cycles = 0U;
    s_app.adc_period_min_cycles = 0xFFFFFFFFUL;
    s_app.adc_period_max_cycles = 0U;
    s_app.adc_period_errors = 0U;
    s_app.adc_sample_min = 0xFFFFU;
    s_app.adc_sample_max = 0U;

    status = TbAdcDma_Start();
    if (status != TB_STATUS_OK)
    {
        App_Write("ADC DMA start status=");
        App_Write(App_StatusText(status));
        App_Write("\r\n");
        return;
    }

    s_app.adc_test_active = true;
    App_Write("ADC DMA started blocks=");
    App_WriteU32(s_app.adc_target_blocks);
    App_Write(" duration_ms=1920\r\n");
}

static void App_ProcessAdcDescriptors(void)
{
    TbAdcDmaDescriptor descriptor;
    TbAdcDmaStats stats;

    if (!s_app.adc_test_active)
    {
        return;
    }

    TbAdcDma_GetStats(&stats);
    if (stats.state == TB_ADC_DMA_STATE_FAULT)
    {
        App_FinishAdcTest(TB_STATUS_HW_ERROR);
        return;
    }

    while (TbAdcDma_PopDescriptor(&descriptor) == TB_STATUS_OK)
    {
        const uint16_t *samples = TbAdcDma_GetBuffer(descriptor.buffer_index);
        uint32_t index;

        if (s_app.adc_blocks_checked != 0U)
        {
            uint32_t period_cycles = descriptor.completed_at_cycles -
                                     s_app.adc_previous_completed_cycles;

            if (period_cycles < s_app.adc_period_min_cycles)
            {
                s_app.adc_period_min_cycles = period_cycles;
            }
            if (period_cycles > s_app.adc_period_max_cycles)
            {
                s_app.adc_period_max_cycles = period_cycles;
            }
            s_app.adc_period_sum_cycles += period_cycles;
            ++s_app.adc_period_count;
            if ((period_cycles < (TB_ADC_EXPECTED_BLOCK_CYCLES -
                                  TB_ADC_BLOCK_TOLERANCE_CYCLES)) ||
                (period_cycles > (TB_ADC_EXPECTED_BLOCK_CYCLES +
                                  TB_ADC_BLOCK_TOLERANCE_CYCLES)))
            {
                ++s_app.adc_period_errors;
            }
        }
        s_app.adc_previous_completed_cycles = descriptor.completed_at_cycles;

        if (descriptor.sequence != s_app.adc_blocks_checked)
        {
            ++s_app.adc_sequence_errors;
        }
        if ((descriptor.buffer_index != (uint8_t)(descriptor.sequence & 1U)) ||
            (descriptor.sample_count != TB_ADC_DMA_SAMPLES_PER_BUFFER) ||
            (samples == 0))
        {
            ++s_app.adc_buffer_errors;
        }
        else
        {
            for (index = 0U; index < descriptor.sample_count; ++index)
            {
                uint16_t sample = samples[index];

                if (sample > 4095U)
                {
                    ++s_app.adc_buffer_errors;
                    break;
                }
                if (sample < s_app.adc_sample_min)
                {
                    s_app.adc_sample_min = sample;
                }
                if (sample > s_app.adc_sample_max)
                {
                    s_app.adc_sample_max = sample;
                }
                s_app.adc_sample_sum += sample;
                ++s_app.adc_samples_checked;
            }
        }

        ++s_app.adc_blocks_checked;
        if (s_app.adc_blocks_checked >= s_app.adc_target_blocks)
        {
            App_FinishAdcTest(TB_STATUS_OK);
            return;
        }
    }
}

static void App_PrintNorInfo(void)
{
    TbNorStats stats;

    TbNor_GetStats(&stats);
    App_Write("nor_jedec manufacturer=");
    App_WriteHex8(stats.manufacturer_id);
    App_Write(" memory_type=");
    App_WriteHex8(stats.memory_type);
    App_Write(" capacity_code=");
    App_WriteHex8(stats.capacity_code);
    App_Write(" capacity_bytes=");
    App_WriteU32(stats.capacity_bytes);
    App_Write(" status1=");
    App_WriteHex8(stats.status_register_1);
    App_Write(" state=");
    App_WriteU32((uint32_t)stats.state);
    App_Write("\r\n");
}

static void App_FinishNorTest(bool passed)
{
    TbNorStats stats;

    TbNor_GetStats(&stats);
    s_app.nor_test_active = false;
    s_app.nor_test_stage = APP_NOR_TEST_IDLE;

    App_PrintNorInfo();
    App_Write("NOR SELFTEST scratch_address=");
    App_WriteHex32(s_app.nor_scratch_address);
    App_Write(" erased_not_ff=");
    App_WriteU32(s_app.nor_erased_not_ff);
    App_Write(" mismatches=");
    App_WriteU32(s_app.nor_mismatch_count);
    App_Write(" page_crc=");
    App_WriteHex32(s_app.nor_page_crc);
    App_Write(" calculated_crc=");
    App_WriteHex32(s_app.nor_calculated_crc);
    App_Write("\r\nprogram_operations=");
    App_WriteU32(stats.page_program_count);
    App_Write(" programmed_bytes=");
    App_WriteU32(stats.programmed_bytes);
    App_Write(" reads=");
    App_WriteU32(stats.read_count);
    App_Write(" read_bytes=");
    App_WriteU32(stats.read_bytes);
    App_Write(" erases=");
    App_WriteU32(stats.sector_erase_count);
    App_Write(" status_polls=");
    App_WriteU32(stats.status_poll_count);
    App_Write("\r\ntimeout_observed=");
    App_WriteU32(s_app.nor_timeout_observed);
    App_Write(" driver_timeouts=");
    App_WriteU32(stats.timeout_count);
    App_Write(" recoveries=");
    App_WriteU32(stats.recovery_count);
    App_Write(" spi_errors=");
    App_WriteU32(stats.spi_error_count);
    App_Write(" wel_errors=");
    App_WriteU32(stats.write_enable_error_count);
    App_Write("\r\n");
    App_Write(passed ? "NOR SELFTEST: PASS\r\n" : "NOR SELFTEST: FAIL\r\n");
}

static bool App_NorStartAccepted(TbStatus status)
{
    if (status == TB_STATUS_OK)
    {
        return true;
    }

    App_Write("NOR next operation start failed status=");
    App_Write(App_StatusText(status));
    App_Write(" stage=");
    App_WriteU32((uint32_t)s_app.nor_test_stage);
    App_Write("\r\n");
    App_FinishNorTest(false);
    return false;
}

static void App_NorSelfTestComplete(TbStatus status, void *user_context)
{
    uint32_t index;

    (void)user_context;
    if (!s_app.nor_test_active)
    {
        return;
    }

    if (s_app.nor_test_stage == APP_NOR_TEST_FORCED_TIMEOUT)
    {
        if (status != TB_STATUS_TIMEOUT)
        {
            App_Write("NOR forced timeout returned status=");
            App_Write(App_StatusText(status));
            App_Write("\r\n");
            App_FinishNorTest(false);
            return;
        }

        s_app.nor_timeout_observed = 1U;
        s_app.nor_test_stage = APP_NOR_TEST_RECOVER;
        (void)App_NorStartAccepted(TbNor_RecoverStart(
            1000U, App_NorSelfTestComplete, 0));
        return;
    }

    if (status != TB_STATUS_OK)
    {
        App_Write("NOR operation failed status=");
        App_Write(App_StatusText(status));
        App_Write(" stage=");
        App_WriteU32((uint32_t)s_app.nor_test_stage);
        App_Write("\r\n");
        App_FinishNorTest(false);
        return;
    }

    switch (s_app.nor_test_stage)
    {
        case APP_NOR_TEST_IDENTIFY:
        {
            TbAdcPageMetadata metadata;
            TbNorStats stats;

            TbNor_GetStats(&stats);
            if ((stats.manufacturer_id != 0xEFU) ||
                (stats.memory_type != 0x40U) ||
                (stats.capacity_code != 0x17U) ||
                (stats.capacity_bytes != 8388608UL))
            {
                App_Write("NOR unexpected JEDEC ID for W25Q64\r\n");
                App_FinishNorTest(false);
                return;
            }

            s_app.nor_scratch_address =
                stats.capacity_bytes - TB_NOR_SECTOR_SIZE_BYTES;
            for (index = 0U; index < TB_PAGE_SAMPLE_COUNT; ++index)
            {
                s_nor_samples[index] =
                    (uint16_t)(((index * 53U) + 7U) & 0x0FFFU);
            }
            metadata.flags = 0x5AU;
            metadata.page_sequence = 0x11223344UL;
            metadata.t0_us = 0x55667788UL;
            metadata.sample_period_ns = 100000UL;
            metadata.capture_id = 0xA1B2C3D4UL;
            if ((TbPage_BuildAdc(s_nor_page, &metadata, s_nor_samples,
                                 TB_PAGE_SAMPLE_COUNT,
                                 &s_app.nor_page_crc) != TB_STATUS_OK) ||
                (TbPage_MarkCommitted(s_nor_page) != TB_STATUS_OK))
            {
                App_Write("NOR selftest page build failed\r\n");
                App_FinishNorTest(false);
                return;
            }

            s_app.nor_test_stage = APP_NOR_TEST_ERASE;
            (void)App_NorStartAccepted(TbNor_SectorEraseStart(
                s_app.nor_scratch_address, 1000U,
                App_NorSelfTestComplete, 0));
            break;
        }

        case APP_NOR_TEST_ERASE:
            s_app.nor_test_stage = APP_NOR_TEST_ERASED_READ;
            (void)App_NorStartAccepted(TbNor_ReadStart(
                s_app.nor_scratch_address, s_nor_readback,
                TB_NOR_PAGE_SIZE_BYTES, App_NorSelfTestComplete, 0));
            break;

        case APP_NOR_TEST_ERASED_READ:
            s_app.nor_erased_not_ff = 0U;
            for (index = 0U; index < TB_NOR_PAGE_SIZE_BYTES; ++index)
            {
                if (s_nor_readback[index] != 0xFFU)
                {
                    ++s_app.nor_erased_not_ff;
                }
            }
            if (s_app.nor_erased_not_ff != 0U)
            {
                App_FinishNorTest(false);
                return;
            }
            s_app.nor_test_stage = APP_NOR_TEST_PROGRAM_BODY;
            (void)App_NorStartAccepted(TbNor_PageProgramStart(
                s_app.nor_scratch_address, s_nor_page,
                TB_PAGE_COMMIT_OFFSET, 100U,
                App_NorSelfTestComplete, 0));
            break;

        case APP_NOR_TEST_PROGRAM_BODY:
            s_app.nor_test_stage = APP_NOR_TEST_PROGRAM_COMMIT;
            (void)App_NorStartAccepted(TbNor_PageProgramStart(
                s_app.nor_scratch_address + TB_PAGE_COMMIT_OFFSET,
                &s_nor_page[TB_PAGE_COMMIT_OFFSET], 1U, 100U,
                App_NorSelfTestComplete, 0));
            break;

        case APP_NOR_TEST_PROGRAM_COMMIT:
            s_app.nor_test_stage = APP_NOR_TEST_VERIFY_READ;
            (void)App_NorStartAccepted(TbNor_ReadStart(
                s_app.nor_scratch_address, s_nor_readback,
                TB_NOR_PAGE_SIZE_BYTES, App_NorSelfTestComplete, 0));
            break;

        case APP_NOR_TEST_VERIFY_READ:
        {
            TbPageValidation validation;

            s_app.nor_mismatch_count = 0U;
            for (index = 0U; index < TB_NOR_PAGE_SIZE_BYTES; ++index)
            {
                if (s_nor_readback[index] != s_nor_page[index])
                {
                    ++s_app.nor_mismatch_count;
                }
            }
            if ((s_app.nor_mismatch_count != 0U) ||
                (TbPage_Validate(s_nor_readback, &validation,
                                 &s_app.nor_calculated_crc) != TB_STATUS_OK) ||
                (validation != TB_PAGE_VALID) ||
                (s_app.nor_calculated_crc != s_app.nor_page_crc))
            {
                App_FinishNorTest(false);
                return;
            }

            s_app.nor_test_stage = APP_NOR_TEST_FORCED_TIMEOUT;
            (void)App_NorStartAccepted(TbNor_SectorEraseStart(
                s_app.nor_scratch_address, 1U,
                App_NorSelfTestComplete, 0));
            break;
        }

        case APP_NOR_TEST_RECOVER:
            App_FinishNorTest((s_app.nor_timeout_observed == 1U));
            break;

        default:
            App_FinishNorTest(false);
            break;
    }
}

static void App_NorIdentifyComplete(TbStatus status, void *user_context)
{
    (void)user_context;
    App_Write("NOR identify status=");
    App_Write(App_StatusText(status));
    App_Write("\r\n");
    App_PrintNorInfo();
}

static void App_StartNorSelfTest(void)
{
    if (s_app.nor_test_active)
    {
        App_Write("NOR selftest status=BUSY\r\n");
        return;
    }

    s_app.nor_test_active = true;
    s_app.nor_test_stage = APP_NOR_TEST_IDENTIFY;
    s_app.nor_scratch_address = 0U;
    s_app.nor_page_crc = 0U;
    s_app.nor_calculated_crc = 0U;
    s_app.nor_erased_not_ff = 0U;
    s_app.nor_mismatch_count = 0U;
    s_app.nor_timeout_observed = 0U;
    App_Write("NOR SELFTEST started; destructive scratch=last sector\r\n");
    (void)App_NorStartAccepted(TbNor_IdentifyStart(
        App_NorSelfTestComplete, 0));
}

static void App_PrintRecorderMetrics(void)
{
    TbRecorderStats stats;

    TbRecorder_GetStats(&stats);

    App_Write("RECORDER state=");
    App_WriteU32((uint32_t)stats.state);
    App_Write(" operation=");
    App_WriteU32((uint32_t)stats.operation);
    App_Write(" source=");
    App_WriteU32((uint32_t)stats.source);
    App_Write(" last_status=");
    App_Write(App_StatusText((TbStatus)stats.last_status));
    App_Write(" capture_id=");
    App_WriteHex32(stats.capture_id);
    App_Write(" target_pages=");
    App_WriteU32(stats.target_pages);
    App_Write(" sectors_required=");
    App_WriteU32(stats.sectors_required);
    App_Write(" sectors_prepared=");
    App_WriteU32(stats.sectors_prepared);
    App_Write(" prepared_pages=");
    App_WriteU32(stats.prepared_capacity_pages);
    App_Write("\r\nadc_blocks=");
    App_WriteU32(stats.adc_blocks_seen);
    App_Write(" pages_packed=");
    App_WriteU32(stats.pages_packed);
    App_Write(" pages_committed=");
    App_WriteU32(stats.pages_committed);
    App_Write(" pages_verified=");
    App_WriteU32(stats.pages_verified);
    App_Write(" capture_ms=");
    App_WriteU32(stats.capture_ms);
    App_Write(" valid_pages=");
    App_WriteU32(stats.valid_pages);
    App_Write(" recovery_scanned=");
    App_WriteU32(stats.recovery_pages_scanned);
    App_Write(" rejected_torn=");
    App_WriteU32(stats.rejected_torn_pages);
    App_Write("\r\nsequence_errors=");
    App_WriteU32(stats.sequence_errors);
    App_Write(" page_crc_errors=");
    App_WriteU32(stats.page_crc_errors);
    App_Write(" flash_errors=");
    App_WriteU32(stats.flash_errors);
    App_Write(" pack_max_cycles=");
    App_WriteU32(stats.pack_max_cycles);
    App_Write("\r\npage_pool_hwm=");
    App_WriteU32(stats.page_pool_high_water);
    App_Write(" page_pool_overflow=");
    App_WriteU32(stats.page_pool_overflow);
    App_Write(" adc_descriptor_hwm=");
    App_WriteU32(stats.adc_descriptor_high_water);
    App_Write(" adc_descriptor_overflow=");
    App_WriteU32(stats.adc_descriptor_overflow);
    App_Write(" adc_dma_errors=");
    App_WriteU32(stats.adc_dma_errors);
    App_Write(" adc_overruns=");
    App_WriteU32(stats.adc_overruns);
    App_Write("\r\ntrace_seen=");
    App_WriteU32(stats.trace_frames_seen);
    App_Write(" trace_accepted=");
    App_WriteU32(stats.trace_frames_accepted);
    App_Write(" trace_rejected=");
    App_WriteU32(stats.trace_frames_rejected);
    App_Write(" trace_crc_errors=");
    App_WriteU32(stats.trace_crc_errors);
    App_Write(" trace_sequence_gaps=");
    App_WriteU32(stats.trace_sequence_gaps);
    App_Write("\r\ningress_hwm=");
    App_WriteU32(stats.ingress_descriptor_high_water);
    App_Write(" ingress_overflow=");
    App_WriteU32(stats.ingress_descriptor_overflow);
    App_Write(" ingress_dma_errors=");
    App_WriteU32(stats.ingress_dma_errors);
    App_Write(" ingress_spi_errors=");
    App_WriteU32(stats.ingress_spi_errors);
    App_Write(" ingress_short=");
    App_WriteU32(stats.ingress_short_frames);
    App_Write(" ingress_recoveries=");
    App_WriteU32(stats.ingress_recoveries);
    App_Write(" ingress_isr_max_cycles=");
    App_WriteU32(stats.ingress_max_isr_cycles);
    App_Write("\r\n");
}

static void App_PrintRecorderProgress(void)
{
    TbRecorderStats stats;
    uint32_t errors;

    TbRecorder_GetStats(&stats);
    errors = stats.sequence_errors + stats.page_crc_errors +
             stats.flash_errors + stats.page_pool_overflow +
             stats.adc_descriptor_overflow + stats.adc_dma_errors +
             stats.adc_overruns + stats.trace_crc_errors +
             stats.trace_sequence_gaps + stats.ingress_descriptor_overflow +
             stats.ingress_dma_errors + stats.ingress_spi_errors +
             stats.ingress_short_frames;
    App_Write("P state=");
    App_WriteU32((uint32_t)stats.state);
    App_Write(" packed=");
    App_WriteU32(stats.pages_packed);
    App_Write(" committed=");
    App_WriteU32(stats.pages_committed);
    App_Write(" hwm=");
    App_WriteU32(stats.page_pool_high_water);
    App_Write(" err=");
    App_WriteU32(errors);
    App_Write("\r\n");
}

static bool App_StorageOperationAllowed(void)
{
    return !s_app.nor_test_active && !s_app.adc_test_active &&
           !s_app.spi_dma_active && !s_app.torn_fault_active &&
           (s_app.dump_stage == APP_DUMP_IDLE) &&
           !TbRecorder_IsBusy();
}

static void App_PrepareComplete(TbStatus status, void *user_context)
{
    TbRecorderStats stats;

    (void)user_context;
    TbRecorder_GetStats(&stats);
    App_Write("PREPARE RESULT status=");
    App_Write(App_StatusText(status));
    App_Write(" target_pages=");
    App_WriteU32(stats.target_pages);
    App_Write(" sectors_prepared=");
    App_WriteU32(stats.sectors_prepared);
    App_Write(" prepared_pages=");
    App_WriteU32(stats.prepared_capacity_pages);
    App_Write("\r\n> ");
}

static void App_RecordComplete(TbStatus status, void *user_context)
{
    TbRecorderStats stats;
    bool passed;

    (void)user_context;
    TbRecorder_GetStats(&stats);
    passed = (status == TB_STATUS_OK) &&
             (stats.state == TB_RECORDER_STATE_COMPLETE) &&
             (stats.pages_packed == stats.target_pages) &&
             (stats.pages_committed == stats.target_pages) &&
             (stats.pages_verified == stats.target_pages) &&
             (stats.valid_pages == stats.target_pages) &&
             (stats.sequence_errors == 0U) &&
             (stats.page_crc_errors == 0U) &&
             (stats.flash_errors == 0U) &&
             (stats.page_pool_overflow == 0U) &&
             (stats.adc_descriptor_overflow == 0U) &&
             (stats.adc_dma_errors == 0U) &&
             (stats.adc_overruns == 0U);

    App_Write("RECORD RESULT status=");
    App_Write(App_StatusText(status));
    App_Write(" result=");
    App_Write(passed ? "PASS" : "FAIL");
    App_Write("\r\n");
    App_PrintRecorderMetrics();
    App_Write("> ");
}

static void App_RecoveryComplete(TbStatus status, void *user_context)
{
    TbRecorderStats stats;

    (void)user_context;
    TbRecorder_GetStats(&stats);
    App_Write("RECOVERY RESULT status=");
    App_Write(App_StatusText(status));
    App_Write(" source=");
    App_Write(s_app.boot_recovery_active ? "BOOT" : "COMMAND");
    App_Write(" valid_pages=");
    App_WriteU32(stats.valid_pages);
    App_Write(" scanned_pages=");
    App_WriteU32(stats.recovery_pages_scanned);
    App_Write(" rejected_torn_pages=");
    App_WriteU32(stats.rejected_torn_pages);
    App_Write(" sequence_errors=");
    App_WriteU32(stats.sequence_errors);
    App_Write(" crc_errors=");
    App_WriteU32(stats.page_crc_errors);
    App_Write(" capture_id=");
    App_WriteHex32(stats.capture_id);
    App_Write("\r\n> ");
    s_app.boot_recovery_active = false;
}

static void App_StartPrepare(uint32_t seconds)
{
    uint32_t pages = App_PagesForSeconds(seconds);
    TbStatus status;

    if (!App_StorageOperationAllowed())
    {
        App_Write("PREPARE start status=BUSY\r\n");
        return;
    }
    status = TbRecorder_Prepare(pages, App_PrepareComplete, 0);
    App_Write("PREPARE start status=");
    App_Write(App_StatusText(status));
    App_Write(" seconds=");
    App_WriteU32(seconds);
    App_Write(" target_pages=");
    App_WriteU32(pages);
    App_Write(" sectors=");
    App_WriteU32((pages + 15U) / 16U);
    App_Write("\r\n");
}

static void App_StartRecord(uint32_t seconds)
{
    uint32_t pages = App_PagesForSeconds(seconds);
    TbStatus status;

    if (!App_StorageOperationAllowed())
    {
        App_Write("RECORD start status=BUSY\r\n");
        return;
    }
    status = TbRecorder_Record(TB_FINAL_CAPTURE_ID, pages, true,
                               App_RecordComplete, 0);
    App_Write("RECORD start status=");
    App_Write(App_StatusText(status));
    App_Write(" seconds=");
    App_WriteU32(seconds);
    App_Write(" target_pages=");
    App_WriteU32(pages);
    App_Write(" target_samples=");
    App_WriteU32(pages * TB_PAGE_SAMPLE_COUNT);
    App_Write(" verify=FULL\r\n");
}

static void App_StartTracePrepare(uint32_t frames)
{
    TbStatus status;

    if (!App_StorageOperationAllowed())
    {
        App_Write("TRACE PREPARE start status=BUSY\r\n");
        return;
    }
    status = TbRecorder_Prepare(frames, App_PrepareComplete, 0);
    App_Write("TRACE PREPARE start status=");
    App_Write(App_StatusText(status));
    App_Write(" target_frames=");
    App_WriteU32(frames);
    App_Write(" sectors=");
    App_WriteU32((frames + 15U) / 16U);
    App_Write("\r\n");
}

static void App_TraceRecordComplete(TbStatus status, void *user_context)
{
    TbRecorderStats stats;
    bool passed;

    (void)user_context;
    TbRecorder_GetStats(&stats);
    passed = (status == TB_STATUS_OK) &&
             (stats.state == TB_RECORDER_STATE_COMPLETE) &&
             (stats.source == TB_RECORDER_SOURCE_TRACE) &&
             (stats.trace_frames_accepted == stats.target_pages) &&
             (stats.pages_committed == stats.target_pages) &&
             (stats.pages_verified == stats.target_pages) &&
             (stats.valid_pages == stats.target_pages) &&
             (stats.sequence_errors == 0U) &&
             (stats.page_crc_errors == 0U) &&
             (stats.flash_errors == 0U) &&
             (stats.page_pool_overflow == 0U) &&
             (stats.ingress_descriptor_overflow == 0U) &&
             (stats.ingress_dma_errors == 0U) &&
             (stats.ingress_spi_errors == 0U) &&
             (stats.ingress_short_frames == 0U);

    App_Write("TRACE RECORD RESULT status=");
    App_Write(App_StatusText(status));
    App_Write(" result=");
    App_Write(passed ? "PASS" : "FAIL");
    App_Write("\r\n");
    App_PrintRecorderMetrics();
    App_Write("> ");
}

static void App_StartTraceRecord(uint32_t frames)
{
    TbStatus status;

    if (!App_StorageOperationAllowed())
    {
        App_Write("TRACE RECORD start status=BUSY\r\n");
        return;
    }
    status = TbRecorder_RecordTrace(TB_TRACE_CAPTURE_ID, frames, true,
                                    App_TraceRecordComplete, 0);
    App_Write("TRACE RECORD start status=");
    App_Write(App_StatusText(status));
    App_Write(" target_frames=");
    App_WriteU32(frames);
    App_Write(" frame_bytes=");
    App_WriteU32(TB_TRACE_FRAME_SIZE);
    App_Write(" verify=FULL\r\n");
}

static void App_StartRecovery(bool boot_recovery)
{
    TbStatus status;

    if (!App_StorageOperationAllowed())
    {
        App_Write("RECOVERY start status=BUSY\r\n");
        return;
    }
    s_app.boot_recovery_active = boot_recovery;
    status = TbRecorder_Recover(TB_RECORDER_MAX_PAGES,
                                App_RecoveryComplete, 0);
    App_Write("RECOVERY start status=");
    App_Write(App_StatusText(status));
    App_Write(" source=");
    App_Write(boot_recovery ? "BOOT" : "COMMAND");
    App_Write(" scan_limit_pages=");
    App_WriteU32(TB_RECORDER_MAX_PAGES);
    App_Write("\r\n");
    if (status != TB_STATUS_OK)
    {
        s_app.boot_recovery_active = false;
    }
}

static void App_DumpReadComplete(TbStatus status, void *user_context)
{
    (void)user_context;
    s_app.dump_status = status;
    s_app.dump_stage = (status == TB_STATUS_OK) ?
                       APP_DUMP_SEND_PAGE : APP_DUMP_ERROR;
}

static void App_StartDump(uint32_t baudrate)
{
    TbNorStats nor_stats;
    uint32_t pages;

    if (!App_StorageOperationAllowed())
    {
        App_Write("DUMP start status=BUSY\r\n");
        return;
    }
    if ((baudrate != 115200U) && (baudrate != 230400U) &&
        (baudrate != 460800U))
    {
        App_Write("DUMP start status=RANGE_ERROR\r\n");
        return;
    }

    TbNor_GetStats(&nor_stats);
    pages = TbRecorder_GetReadablePages();
    if ((nor_stats.state != TB_NOR_STATE_READY) ||
        (pages > TB_RECORDER_MAX_PAGES))
    {
        App_Write("DUMP start status=STATE_ERROR\r\n");
        return;
    }

    s_app.dump_page_count = pages;
    s_app.dump_page_index = 0U;
    s_app.dump_baudrate = baudrate;
    s_app.dump_bytes_sent = 0U;
    s_app.dump_status = TB_STATUS_BUSY;
    s_app.dump_stage = APP_DUMP_ANNOUNCE;
    App_Write("DUMP start status=OK pages=");
    App_WriteU32(pages);
    App_Write(" baud=");
    App_WriteU32(baudrate);
    App_Write("\r\n");
}

static void App_DumpFail(TbStatus status)
{
    s_app.dump_status = status;
    s_app.dump_stage = APP_DUMP_ERROR;
}

static void App_ServiceDump(uint32_t now_ms)
{
    TbStatus status;

    switch (s_app.dump_stage)
    {
        case APP_DUMP_IDLE:
        case APP_DUMP_READ_WAIT:
            break;

        case APP_DUMP_ANNOUNCE:
            App_Write("TB_DUMP_BEGIN ");
            App_WriteU32(s_app.dump_page_count);
            App_Write(" 256 ");
            App_WriteU32(s_app.dump_baudrate);
            App_Write("\r\n");
            status = TbUart_SetBaudrate(s_app.dump_baudrate);
            if (status != TB_STATUS_OK)
            {
                App_DumpFail(status);
            }
            else
            {
                s_app.dump_deadline_ms = now_ms + TB_DUMP_SWITCH_DELAY_MS;
                s_app.dump_stage = APP_DUMP_SWITCH_DELAY;
            }
            break;

        case APP_DUMP_SWITCH_DELAY:
            if (App_TimeReached(now_ms, s_app.dump_deadline_ms))
            {
                s_app.dump_stage = (s_app.dump_page_count == 0U) ?
                                   APP_DUMP_END : APP_DUMP_READ_START;
            }
            break;

        case APP_DUMP_READ_START:
            status = TbNor_ReadStart(
                TB_RECORDER_DATA_BASE_ADDRESS +
                    (s_app.dump_page_index * TB_NOR_PAGE_SIZE_BYTES),
                s_app.dump_page, TB_NOR_PAGE_SIZE_BYTES,
                App_DumpReadComplete, 0);
            if (status == TB_STATUS_OK)
            {
                s_app.dump_stage = APP_DUMP_READ_WAIT;
            }
            else
            {
                App_DumpFail(status);
            }
            break;

        case APP_DUMP_SEND_PAGE:
            status = TbUart_Write(s_app.dump_page,
                                  TB_NOR_PAGE_SIZE_BYTES);
            if (status != TB_STATUS_OK)
            {
                App_DumpFail(status);
                break;
            }
            s_app.dump_bytes_sent += TB_NOR_PAGE_SIZE_BYTES;
            ++s_app.dump_page_index;
            s_app.dump_stage =
                (s_app.dump_page_index < s_app.dump_page_count) ?
                APP_DUMP_READ_START : APP_DUMP_END;
            break;

        case APP_DUMP_END:
            App_Write("\r\nTB_DUMP_END\r\n");
            status = TbUart_WaitTxComplete();
            if (status != TB_STATUS_OK)
            {
                App_DumpFail(status);
            }
            else
            {
                s_app.dump_deadline_ms = now_ms + TB_DUMP_SWITCH_DELAY_MS;
                s_app.dump_stage = APP_DUMP_RESTORE_DELAY;
            }
            break;

        case APP_DUMP_RESTORE_DELAY:
            if (App_TimeReached(now_ms, s_app.dump_deadline_ms))
            {
                status = TbUart_SetBaudrate(TB_CONTROL_BAUDRATE);
                if (status != TB_STATUS_OK)
                {
                    App_DumpFail(status);
                }
                else
                {
                    s_app.dump_status = TB_STATUS_OK;
                    s_app.dump_stage = APP_DUMP_RESULT;
                }
            }
            break;

        case APP_DUMP_RESULT:
            App_Write("DUMP RESULT status=OK pages=");
            App_WriteU32(s_app.dump_page_index);
            App_Write(" bytes=");
            App_WriteU32(s_app.dump_bytes_sent);
            App_Write("\r\n> ");
            s_app.dump_stage = APP_DUMP_IDLE;
            break;

        case APP_DUMP_ERROR:
        {
            TbUartStats uart_stats;

            TbUart_GetStats(&uart_stats);
            if (uart_stats.baudrate != TB_CONTROL_BAUDRATE)
            {
                (void)TbUart_SetBaudrate(TB_CONTROL_BAUDRATE);
            }
            App_Write("DUMP RESULT status=");
            App_Write(App_StatusText(s_app.dump_status));
            App_Write(" pages=");
            App_WriteU32(s_app.dump_page_index);
            App_Write(" bytes=");
            App_WriteU32(s_app.dump_bytes_sent);
            App_Write("\r\n> ");
            s_app.dump_stage = APP_DUMP_IDLE;
            break;
        }

        default:
            App_DumpFail(TB_STATUS_STATE_ERROR);
            break;
    }
}

static void App_TornFaultComplete(TbStatus status, void *user_context)
{
    (void)user_context;
    s_app.torn_fault_active = false;
    App_Write("FAULT TORN RESULT status=");
    App_Write(App_StatusText(status));
    App_Write(" page=");
    App_WriteU32(s_app.torn_fault_page);
    App_Write(" commit_written=0 reset_required=");
    App_Write((status == TB_STATUS_OK) ? "1" : "0");
    App_Write("\r\n> ");
}

static void App_StartTornFault(void)
{
    TbRecorderStats stats;
    TbAdcPageMetadata metadata;
    uint32_t page_crc;
    uint32_t index;
    TbStatus status;

    if (!App_StorageOperationAllowed())
    {
        App_Write("FAULT TORN start status=BUSY\r\n");
        return;
    }
    TbRecorder_GetStats(&stats);
    if ((stats.valid_pages == 0U) ||
        (stats.valid_pages >= TB_RECORDER_MAX_PAGES))
    {
        App_Write("FAULT TORN start status=STATE_ERROR\r\n");
        return;
    }

    for (index = 0U; index < TB_PAGE_SAMPLE_COUNT; ++index)
    {
        s_nor_samples[index] = (uint16_t)((index * 29U) & 0x0FFFU);
    }
    metadata.flags = 0x80U;
    metadata.page_sequence = stats.valid_pages;
    metadata.t0_us = stats.valid_pages * 9600UL;
    metadata.sample_period_ns = 100000UL;
    metadata.capture_id = stats.capture_id;
    status = TbPage_BuildAdc(s_nor_page, &metadata, s_nor_samples,
                             TB_PAGE_SAMPLE_COUNT, &page_crc);
    if (status != TB_STATUS_OK)
    {
        App_Write("FAULT TORN start status=");
        App_Write(App_StatusText(status));
        App_Write("\r\n");
        return;
    }

    s_app.torn_fault_page = stats.valid_pages;
    status = TbNor_PageProgramStart(
        TB_RECORDER_DATA_BASE_ADDRESS +
            (s_app.torn_fault_page * TB_NOR_PAGE_SIZE_BYTES),
        s_nor_page, TB_PAGE_COMMIT_OFFSET, 100U,
        App_TornFaultComplete, 0);
    if (status == TB_STATUS_OK)
    {
        s_app.torn_fault_active = true;
    }
    App_Write("FAULT TORN start status=");
    App_Write(App_StatusText(status));
    App_Write(" page=");
    App_WriteU32(s_app.torn_fault_page);
    App_Write(" body_bytes=255 commit_written=0\r\n");
}

static void App_RecorderComplete(TbStatus status, void *user_context)
{
    TbRecorderStats stats;
    bool passed;

    (void)user_context;
    TbRecorder_GetStats(&stats);
    passed = (status == TB_STATUS_OK) &&
             (stats.state == TB_RECORDER_STATE_COMPLETE) &&
             (stats.sectors_prepared == stats.sectors_required) &&
             (stats.adc_blocks_seen == stats.target_pages) &&
             (stats.pages_packed == stats.target_pages) &&
             (stats.pages_committed == stats.target_pages) &&
             (stats.pages_verified == stats.target_pages) &&
             (stats.sequence_errors == 0U) &&
             (stats.page_crc_errors == 0U) &&
             (stats.flash_errors == 0U) &&
             (stats.page_pool_overflow == 0U) &&
             (stats.adc_descriptor_overflow == 0U) &&
             (stats.adc_dma_errors == 0U) &&
             (stats.adc_overruns == 0U) &&
             (stats.page_pool_high_water > 0U) &&
             (stats.page_pool_high_water < 8U) &&
             (stats.pack_max_cycles < TB_ADC_EXPECTED_BLOCK_CYCLES);

    App_Write("RECORDER completion_status=");
    App_Write(App_StatusText(status));
    App_Write("\r\n");
    App_PrintRecorderMetrics();
    App_Write(passed ? "RECORDER SELFTEST: PASS\r\n" :
                       "RECORDER SELFTEST: FAIL\r\n");
}

static void App_StartRecorderSelfTest(void)
{
    TbStatus status;

    if (s_app.nor_test_active || s_app.adc_test_active)
    {
        App_Write("RECORDER start status=BUSY app_test_active\r\n");
        return;
    }

    status = TbRecorder_Start(0x20260913UL, 200U,
                              App_RecorderComplete, 0);
    App_Write("RECORDER start status=");
    App_Write(App_StatusText(status));
    App_Write(" target_pages=200 data_bytes=51200 capture_ms=1920\r\n");
}

static void App_PrintHelp(void)
{
    App_Write("V2: trace prepare <1..10000> | trace record <1..10000> | ");
    App_Write("trace metrics | trace ingress\r\n");
    App_Write("P0: prepare <1..60> | record <1..60> | recover | ");
    App_Write("dump [115200|230400|460800] | record progress/metrics | bench | ");
    App_Write("fault torn\r\n");
    App_Write("diagnostic: help | ping | info | pins | stats | dwt | ");
    App_Write("led on | led off | led toggle | led auto | spi info | ");
    App_Write("spi loop 16/256/4096/all | spi dma 16/256/4096/all | ");
    App_Write("spi dma timeout | core selftest | adc info | adc test | ");
    App_Write("nor id | nor info | nor selftest | record selftest | ");
    App_Write("record metrics\r\n");
}

static void App_ProcessCommand(void)
{
    char *command = s_app.line;
    uint32_t seconds;
    uint32_t count;

    while (*command == ' ')
    {
        ++command;
    }

    ++s_app.command_count;

    if ((*command == '\0') || App_StringEquals(command, "help"))
    {
        App_PrintHelp();
    }
    else if (App_StringEquals(command, "ping"))
    {
        App_Write("PONG\r\n");
    }
    else if (App_StringEquals(command, "info"))
    {
        App_PrintInfo();
    }
    else if (App_StringEquals(command, "pins"))
    {
        App_PrintPins();
    }
    else if (App_StringEquals(command, "stats"))
    {
        App_PrintStats();
    }
    else if (App_StringEquals(command, "dwt"))
    {
        s_app.dwt_cycles_10ms = App_MeasureDwt10ms();
        App_Write("dwt_10ms_cycles=");
        App_WriteU32(s_app.dwt_cycles_10ms);
        App_Write("\r\n");
    }
    else if (App_StringEquals(command, "spi info"))
    {
        App_PrintSpiInfo();
    }
    else if (App_StringEquals(command, "spi loop 16"))
    {
        (void)App_RunSpiLoopback(16U);
    }
    else if (App_StringEquals(command, "spi loop 256"))
    {
        (void)App_RunSpiLoopback(256U);
    }
    else if (App_StringEquals(command, "spi loop 4096"))
    {
        (void)App_RunSpiLoopback(4096U);
    }
    else if (App_StringEquals(command, "spi loop all"))
    {
        bool passed = App_RunSpiLoopback(16U);
        passed = App_RunSpiLoopback(256U) && passed;
        passed = App_RunSpiLoopback(4096U) && passed;
        App_Write(passed ? "SPI LOOPBACK SUITE: PASS\r\n" :
                           "SPI LOOPBACK SUITE: FAIL\r\n");
    }
    else if (App_StringEquals(command, "spi dma 16"))
    {
        s_app.spi_dma_suite_active = false;
        (void)App_StartSpiDma(16U);
    }
    else if (App_StringEquals(command, "spi dma 256"))
    {
        s_app.spi_dma_suite_active = false;
        (void)App_StartSpiDma(256U);
    }
    else if (App_StringEquals(command, "spi dma 4096"))
    {
        s_app.spi_dma_suite_active = false;
        (void)App_StartSpiDma(4096U);
    }
    else if (App_StringEquals(command, "spi dma all"))
    {
        if (s_app.spi_dma_active)
        {
            App_Write("SPI DMA start status=BUSY\r\n");
        }
        else
        {
            s_app.spi_dma_suite_active = true;
            s_app.spi_dma_suite_passed = true;
            s_app.spi_dma_next_test = 1U;
            if (!App_StartSpiDma(s_spi_test_lengths[0]))
            {
                s_app.spi_dma_suite_active = false;
                App_Write("SPI DMA LOOPBACK SUITE: FAIL start\r\n");
            }
        }
    }
    else if (App_StringEquals(command, "spi dma timeout"))
    {
        s_app.spi_dma_suite_active = false;
        (void)App_StartSpiDmaWithTimeout(4096U, TB_SPI_FAULT_TIMEOUT_CYCLES);
    }
    else if (App_StringEquals(command, "core selftest"))
    {
        App_RunCoreSelfTest();
    }
    else if (App_StringEquals(command, "adc info"))
    {
        App_PrintAdcInfo();
    }
    else if (App_StringEquals(command, "adc test"))
    {
        App_StartAdcTest();
    }
    else if (App_StringEquals(command, "nor id"))
    {
        TbStatus status = TbNor_IdentifyStart(App_NorIdentifyComplete, 0);

        App_Write("NOR identify start status=");
        App_Write(App_StatusText(status));
        App_Write("\r\n");
    }
    else if (App_StringEquals(command, "nor info"))
    {
        App_PrintNorInfo();
    }
    else if (App_StringEquals(command, "nor selftest"))
    {
        App_StartNorSelfTest();
    }
    else if (App_ParseCount(command, "trace prepare",
                            TB_RECORDER_MAX_PAGES, &count))
    {
        App_StartTracePrepare(count);
    }
    else if (App_ParseCount(command, "trace record",
                            TB_RECORDER_MAX_PAGES, &count))
    {
        App_StartTraceRecord(count);
    }
    else if (App_StringEquals(command, "trace metrics"))
    {
        App_PrintRecorderMetrics();
    }
    else if (App_StringEquals(command, "trace ingress"))
    {
        App_PrintStats();
    }
    else if (App_ParseSeconds(command, "prepare", &seconds))
    {
        App_StartPrepare(seconds);
    }
    else if (App_ParseSeconds(command, "record", &seconds))
    {
        App_StartRecord(seconds);
    }
    else if (App_StringEquals(command, "recover"))
    {
        App_StartRecovery(false);
    }
    else if (App_StringEquals(command, "dump"))
    {
        App_StartDump(TB_DEFAULT_DUMP_BAUD);
    }
    else if (App_StringEquals(command, "dump 115200"))
    {
        App_StartDump(115200U);
    }
    else if (App_StringEquals(command, "dump 230400"))
    {
        App_StartDump(230400U);
    }
    else if (App_StringEquals(command, "dump 460800"))
    {
        App_StartDump(460800U);
    }
    else if (App_StringEquals(command, "bench"))
    {
        App_Write("BENCH source=hardware_verified_loopback sck_hz=1312500 ");
        App_Write("length=4096 polling_Bps=149747 dma_Bps=164060 ");
        App_Write("dma_launch_cycles=302 foreground_iterations=23881\r\n");
    }
    else if (App_StringEquals(command, "fault torn"))
    {
        App_StartTornFault();
    }
    else if (App_StringEquals(command, "record selftest"))
    {
        App_StartRecorderSelfTest();
    }
    else if (App_StringEquals(command, "record metrics"))
    {
        App_PrintRecorderMetrics();
    }
    else if (App_StringEquals(command, "record progress"))
    {
        App_PrintRecorderProgress();
    }
    else if (App_StringEquals(command, "led on"))
    {
        s_app.heartbeat_enabled = false;
        TbBoard_SetLed(TB_LED_ON);
        App_Write("OK led=on heartbeat=off\r\n");
    }
    else if (App_StringEquals(command, "led off"))
    {
        s_app.heartbeat_enabled = false;
        TbBoard_SetLed(TB_LED_OFF);
        App_Write("OK led=off heartbeat=off\r\n");
    }
    else if (App_StringEquals(command, "led toggle"))
    {
        s_app.heartbeat_enabled = false;
        TbBoard_ToggleLed();
        App_Write("OK led toggled heartbeat=off\r\n");
    }
    else if (App_StringEquals(command, "led auto"))
    {
        s_app.heartbeat_enabled = true;
        s_app.last_heartbeat_ms = HAL_GetTick();
        App_Write("OK heartbeat=on\r\n");
    }
    else
    {
        ++s_app.unknown_command_count;
        App_Write("ERR unknown command; type help\r\n");
    }
}

static void App_ConsumeByte(uint8_t byte)
{
    if ((byte == '\r') || (byte == '\n'))
    {
        if ((byte == '\n') && s_app.previous_was_cr)
        {
            s_app.previous_was_cr = false;
            return;
        }

        s_app.previous_was_cr = (byte == '\r');
        App_Write("\r\n");

        if (s_app.line_discard)
        {
            ++s_app.line_overflow_count;
            App_Write("ERR line too long (max 63 bytes)\r\n");
        }
        else
        {
            s_app.line[s_app.line_length] = '\0';
            App_ProcessCommand();
        }

        s_app.line_length = 0U;
        s_app.line_discard = false;
        App_Write("> ");
        return;
    }

    s_app.previous_was_cr = false;

    if ((byte == 0x08U) || (byte == 0x7FU))
    {
        if ((!s_app.line_discard) && (s_app.line_length > 0U))
        {
            --s_app.line_length;
            App_Write("\b \b");
        }
        return;
    }

    if ((byte < 0x20U) || (byte > 0x7EU))
    {
        return;
    }

    (void)TbUart_Write(&byte, 1U);
    if (s_app.line_discard)
    {
        return;
    }

    if (s_app.line_length >= (TB_LINE_CAPACITY - 1U))
    {
        s_app.line_discard = true;
        return;
    }

    s_app.line[s_app.line_length++] = (char)byte;
}

void BringupApp_Init(uint32_t reset_flags)
{
    s_app.reset_flags = reset_flags;
    s_app.heartbeat_enabled = true;
    s_app.last_heartbeat_ms = HAL_GetTick();
    s_app.dwt_cycles_10ms = App_MeasureDwt10ms();
    s_app.dump_stage = APP_DUMP_IDLE;
    s_app.boot_recovery_active = false;
    s_app.torn_fault_active = false;

    App_Write("\r\n========================================\r\n");
    App_Write("TraceBox-RT V2 dual-SPI runtime black box v2.0.0\r\n");
    App_Write("SPI2 RX DMA ingress + SPI1 DMA NOR + recovery/dump\r\n");
    App_Write("========================================\r\n");
    App_PrintInfo();

    if ((HAL_RCC_GetSysClockFreq() == 168000000U) &&
        (s_app.dwt_cycles_10ms >= 1600000U) &&
        (s_app.dwt_cycles_10ms <= 1760000U))
    {
        App_Write("SELFTEST: PASS clock+dwt\r\n");
    }
    else
    {
        App_Write("SELFTEST: FAIL clock/dwt; keep this log\r\n");
    }

    App_PrintPins();
    App_PrintHelp();
    App_Write("> ");
    App_StartRecovery(true);
}

void BringupApp_MainFunction(uint32_t now_ms)
{
    uint8_t byte;

    if (s_app.spi_dma_active)
    {
        ++s_app.spi_dma_foreground_iterations;
    }

    App_ProcessAdcDescriptors();

    while ((s_app.dump_stage == APP_DUMP_IDLE) && TbUart_ReadByte(&byte))
    {
        App_ConsumeByte(byte);
    }

    App_ServiceDump(now_ms);

    if (s_app.heartbeat_enabled &&
        ((uint32_t)(now_ms - s_app.last_heartbeat_ms) >= TB_HEARTBEAT_PERIOD_MS))
    {
        s_app.last_heartbeat_ms = now_ms;
        TbBoard_ToggleLed();
    }
}
