#include "tracebox_nor.h"

#include <stdbool.h>
#include <stddef.h>

#include "stm32f407xx.h"
#include "tracebox_spi.h"

#define TB_NOR_CMD_WRITE_ENABLE   0x06U
#define TB_NOR_CMD_READ_STATUS_1  0x05U
#define TB_NOR_CMD_READ_DATA      0x03U
#define TB_NOR_CMD_PAGE_PROGRAM   0x02U
#define TB_NOR_CMD_SECTOR_ERASE   0x20U
#define TB_NOR_CMD_JEDEC_ID       0x9FU
#define TB_NOR_SR1_WIP            (1U << 0U)
#define TB_NOR_SR1_WEL            (1U << 1U)
#define TB_NOR_SPI_TIMEOUT_CYCLES 16800000UL
#define TB_NOR_ID_TIMEOUT_MS      100U
#define TB_NOR_POLL_INTERVAL_MS   1U
#define TB_NOR_COMMAND_CAPACITY   (4U + TB_NOR_PAGE_SIZE_BYTES)

typedef enum
{
    TB_NOR_OPERATION_NONE = 0,
    TB_NOR_OPERATION_IDENTIFY,
    TB_NOR_OPERATION_ERASE,
    TB_NOR_OPERATION_PROGRAM,
    TB_NOR_OPERATION_READ,
    TB_NOR_OPERATION_RECOVER
} TbNorOperation;

typedef enum
{
    TB_NOR_PHASE_IDLE = 0,
    TB_NOR_PHASE_IDENTIFY_LAUNCH,
    TB_NOR_PHASE_IDENTIFY_WAIT,
    TB_NOR_PHASE_WRITE_ENABLE_LAUNCH,
    TB_NOR_PHASE_WRITE_ENABLE_WAIT,
    TB_NOR_PHASE_WEL_POLL_LAUNCH,
    TB_NOR_PHASE_WEL_POLL_WAIT,
    TB_NOR_PHASE_COMMAND_LAUNCH,
    TB_NOR_PHASE_COMMAND_WAIT,
    TB_NOR_PHASE_BUSY_POLL_LAUNCH,
    TB_NOR_PHASE_BUSY_POLL_WAIT,
    TB_NOR_PHASE_READ_LAUNCH,
    TB_NOR_PHASE_READ_WAIT
} TbNorPhase;

typedef struct
{
    TbNorStats stats;
    TbNorOperation operation;
    TbNorPhase phase;
    uint8_t tx[TB_NOR_COMMAND_CAPACITY];
    uint8_t rx[TB_NOR_COMMAND_CAPACITY];
    uint8_t program_data[TB_NOR_PAGE_SIZE_BYTES];
    uint8_t *read_destination;
    uint32_t address;
    uint32_t deadline_ms;
    uint32_t next_poll_ms;
    uint32_t operation_timeout_ms;
    uint16_t data_length;
    volatile bool spi_done;
    volatile TbStatus spi_status;
    TbNorCompletionCallback callback;
    void *user_context;
} TbNorContext;

static TbNorContext s_nor;

static void Nor_Finish(TbStatus status);

static bool Nor_TimeReached(uint32_t now_ms, uint32_t deadline_ms)
{
    return ((int32_t)(now_ms - deadline_ms) >= 0);
}

static bool Nor_AddressRangeValid(uint32_t address, uint32_t length)
{
    uint32_t limit = s_nor.stats.capacity_bytes;

    if ((length == 0U) || (address > 0x00FFFFFFUL) ||
        (length > (0x01000000UL - address)))
    {
        return false;
    }
    return (limit == 0U) || ((address < limit) && (length <= (limit - address)));
}

static void Nor_SpiComplete(TbStatus status, void *user_context)
{
    (void)user_context;
    s_nor.spi_status = status;
    __DMB();
    s_nor.spi_done = true;
}

static TbStatus Nor_StartSpi(uint16_t length)
{
    TbSpiDmaTransfer transfer;
    TbStatus status;

    transfer.tx_data = s_nor.tx;
    transfer.rx_data = s_nor.rx;
    transfer.length = length;
    transfer.timeout_cycles = TB_NOR_SPI_TIMEOUT_CYCLES;
    transfer.completion_callback = Nor_SpiComplete;
    transfer.user_context = 0;
    s_nor.spi_done = false;
    s_nor.spi_status = TB_STATUS_BUSY;
    status = TbSpi_TransferDmaStart(&transfer);
    if ((status != TB_STATUS_OK) && (status != TB_STATUS_BUSY))
    {
        Nor_Finish(status);
    }
    return status;
}

static void Nor_Finish(TbStatus status)
{
    TbNorCompletionCallback callback = s_nor.callback;
    void *user_context = s_nor.user_context;

    if (status == TB_STATUS_OK)
    {
        s_nor.stats.state = TB_NOR_STATE_READY;
    }
    else
    {
        s_nor.stats.state = TB_NOR_STATE_FAULT;
        if (status == TB_STATUS_TIMEOUT)
        {
            ++s_nor.stats.timeout_count;
        }
        else if (status == TB_STATUS_HW_ERROR)
        {
            ++s_nor.stats.spi_error_count;
        }
    }

    s_nor.operation = TB_NOR_OPERATION_NONE;
    s_nor.phase = TB_NOR_PHASE_IDLE;
    s_nor.callback = 0;
    s_nor.user_context = 0;
    if (callback != 0)
    {
        callback(status, user_context);
    }
}

static TbStatus Nor_Begin(TbNorOperation operation,
                          TbNorPhase phase,
                          TbNorCompletionCallback callback,
                          void *user_context)
{
    if (s_nor.stats.state != TB_NOR_STATE_READY)
    {
        return TB_STATUS_BUSY;
    }

    s_nor.operation = operation;
    s_nor.phase = phase;
    s_nor.callback = callback;
    s_nor.user_context = user_context;
    s_nor.spi_done = false;
    s_nor.deadline_ms = 0U;
    s_nor.next_poll_ms = 0U;
    s_nor.stats.state = TB_NOR_STATE_BUSY;
    return TB_STATUS_OK;
}

static bool Nor_ConsumeSpiResult(void)
{
    if (!s_nor.spi_done)
    {
        return false;
    }

    __DMB();
    s_nor.spi_done = false;
    if (s_nor.spi_status != TB_STATUS_OK)
    {
        Nor_Finish(s_nor.spi_status);
        return false;
    }
    return true;
}

static void Nor_PrepareAddressCommand(uint8_t command)
{
    s_nor.tx[0] = command;
    s_nor.tx[1] = (uint8_t)(s_nor.address >> 16U);
    s_nor.tx[2] = (uint8_t)(s_nor.address >> 8U);
    s_nor.tx[3] = (uint8_t)s_nor.address;
}

TbStatus TbNor_Init(void)
{
    s_nor.stats.state = TB_NOR_STATE_READY;
    s_nor.stats.manufacturer_id = 0U;
    s_nor.stats.memory_type = 0U;
    s_nor.stats.capacity_code = 0U;
    s_nor.stats.status_register_1 = 0U;
    s_nor.stats.capacity_bytes = 0U;
    s_nor.stats.identify_count = 0U;
    s_nor.stats.sector_erase_count = 0U;
    s_nor.stats.page_program_count = 0U;
    s_nor.stats.read_count = 0U;
    s_nor.stats.programmed_bytes = 0U;
    s_nor.stats.read_bytes = 0U;
    s_nor.stats.status_poll_count = 0U;
    s_nor.stats.spi_error_count = 0U;
    s_nor.stats.timeout_count = 0U;
    s_nor.stats.write_enable_error_count = 0U;
    s_nor.stats.recovery_count = 0U;
    s_nor.operation = TB_NOR_OPERATION_NONE;
    s_nor.phase = TB_NOR_PHASE_IDLE;
    s_nor.spi_done = false;
    s_nor.callback = 0;
    s_nor.user_context = 0;
    return TB_STATUS_OK;
}

TbStatus TbNor_IdentifyStart(TbNorCompletionCallback callback,
                             void *user_context)
{
    TbStatus status = Nor_Begin(TB_NOR_OPERATION_IDENTIFY,
                               TB_NOR_PHASE_IDENTIFY_LAUNCH,
                               callback, user_context);
    if (status == TB_STATUS_OK)
    {
        s_nor.operation_timeout_ms = TB_NOR_ID_TIMEOUT_MS;
    }
    return status;
}

TbStatus TbNor_SectorEraseStart(uint32_t address,
                                uint32_t timeout_ms,
                                TbNorCompletionCallback callback,
                                void *user_context)
{
    TbStatus status;

    if ((timeout_ms == 0U) ||
        ((address & (TB_NOR_SECTOR_SIZE_BYTES - 1U)) != 0U) ||
        !Nor_AddressRangeValid(address, TB_NOR_SECTOR_SIZE_BYTES))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    status = Nor_Begin(TB_NOR_OPERATION_ERASE,
                       TB_NOR_PHASE_WRITE_ENABLE_LAUNCH,
                       callback, user_context);
    if (status == TB_STATUS_OK)
    {
        s_nor.address = address;
        s_nor.operation_timeout_ms = timeout_ms;
    }
    return status;
}

TbStatus TbNor_PageProgramStart(uint32_t address,
                                const uint8_t *data,
                                uint16_t length,
                                uint32_t timeout_ms,
                                TbNorCompletionCallback callback,
                                void *user_context)
{
    uint16_t index;
    TbStatus status;

    if ((data == 0) || (length == 0U) ||
        (length > TB_NOR_PAGE_SIZE_BYTES) || (timeout_ms == 0U) ||
        (((address & (TB_NOR_PAGE_SIZE_BYTES - 1U)) + length) >
         TB_NOR_PAGE_SIZE_BYTES) || !Nor_AddressRangeValid(address, length))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    status = Nor_Begin(TB_NOR_OPERATION_PROGRAM,
                       TB_NOR_PHASE_WRITE_ENABLE_LAUNCH,
                       callback, user_context);
    if (status == TB_STATUS_OK)
    {
        s_nor.address = address;
        s_nor.data_length = length;
        s_nor.operation_timeout_ms = timeout_ms;
        for (index = 0U; index < length; ++index)
        {
            s_nor.program_data[index] = data[index];
        }
    }
    return status;
}

TbStatus TbNor_ReadStart(uint32_t address,
                         uint8_t *data_out,
                         uint16_t length,
                         TbNorCompletionCallback callback,
                         void *user_context)
{
    TbStatus status;

    if ((data_out == 0) || (length == 0U) ||
        (length > TB_NOR_PAGE_SIZE_BYTES) ||
        !Nor_AddressRangeValid(address, length))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    status = Nor_Begin(TB_NOR_OPERATION_READ,
                       TB_NOR_PHASE_READ_LAUNCH,
                       callback, user_context);
    if (status == TB_STATUS_OK)
    {
        s_nor.address = address;
        s_nor.data_length = length;
        s_nor.read_destination = data_out;
        s_nor.operation_timeout_ms = TB_NOR_ID_TIMEOUT_MS;
    }
    return status;
}

TbStatus TbNor_RecoverStart(uint32_t timeout_ms,
                            TbNorCompletionCallback callback,
                            void *user_context)
{
    if ((timeout_ms == 0U) ||
        ((s_nor.stats.state != TB_NOR_STATE_FAULT) &&
         (s_nor.stats.state != TB_NOR_STATE_READY)))
    {
        return TB_STATUS_STATE_ERROR;
    }

    s_nor.operation = TB_NOR_OPERATION_RECOVER;
    s_nor.phase = TB_NOR_PHASE_BUSY_POLL_LAUNCH;
    s_nor.callback = callback;
    s_nor.user_context = user_context;
    s_nor.operation_timeout_ms = timeout_ms;
    s_nor.deadline_ms = 0U;
    s_nor.next_poll_ms = 0U;
    s_nor.spi_done = false;
    s_nor.stats.state = TB_NOR_STATE_BUSY;
    return TB_STATUS_OK;
}

void TbNor_MainFunction(uint32_t now_ms)
{
    uint16_t index;

    if (s_nor.stats.state != TB_NOR_STATE_BUSY)
    {
        return;
    }

    if ((s_nor.deadline_ms != 0U) && Nor_TimeReached(now_ms, s_nor.deadline_ms))
    {
        Nor_Finish(TB_STATUS_TIMEOUT);
        return;
    }

    switch (s_nor.phase)
    {
        case TB_NOR_PHASE_IDENTIFY_LAUNCH:
            s_nor.tx[0] = TB_NOR_CMD_JEDEC_ID;
            s_nor.tx[1] = 0xFFU;
            s_nor.tx[2] = 0xFFU;
            s_nor.tx[3] = 0xFFU;
            s_nor.deadline_ms = now_ms + s_nor.operation_timeout_ms;
            if (Nor_StartSpi(4U) == TB_STATUS_OK)
            {
                s_nor.phase = TB_NOR_PHASE_IDENTIFY_WAIT;
            }
            break;

        case TB_NOR_PHASE_IDENTIFY_WAIT:
            if (Nor_ConsumeSpiResult())
            {
                uint8_t manufacturer = s_nor.rx[1];
                uint8_t memory_type = s_nor.rx[2];
                uint8_t capacity_code = s_nor.rx[3];
                bool all_zero = (manufacturer == 0x00U) &&
                                (memory_type == 0x00U) &&
                                (capacity_code == 0x00U);
                bool all_one = (manufacturer == 0xFFU) &&
                               (memory_type == 0xFFU) &&
                               (capacity_code == 0xFFU);

                if (all_zero || all_one ||
                    (capacity_code < 16U) || (capacity_code > 24U))
                {
                    Nor_Finish(TB_STATUS_VERIFY_FAILED);
                }
                else
                {
                    s_nor.stats.manufacturer_id = manufacturer;
                    s_nor.stats.memory_type = memory_type;
                    s_nor.stats.capacity_code = capacity_code;
                    s_nor.stats.capacity_bytes = 1UL << capacity_code;
                    ++s_nor.stats.identify_count;
                    Nor_Finish(TB_STATUS_OK);
                }
            }
            break;

        case TB_NOR_PHASE_WRITE_ENABLE_LAUNCH:
            s_nor.tx[0] = TB_NOR_CMD_WRITE_ENABLE;
            if (Nor_StartSpi(1U) == TB_STATUS_OK)
            {
                s_nor.phase = TB_NOR_PHASE_WRITE_ENABLE_WAIT;
            }
            break;

        case TB_NOR_PHASE_WRITE_ENABLE_WAIT:
            if (Nor_ConsumeSpiResult())
            {
                s_nor.phase = TB_NOR_PHASE_WEL_POLL_LAUNCH;
            }
            break;

        case TB_NOR_PHASE_WEL_POLL_LAUNCH:
            s_nor.tx[0] = TB_NOR_CMD_READ_STATUS_1;
            s_nor.tx[1] = 0xFFU;
            if (Nor_StartSpi(2U) == TB_STATUS_OK)
            {
                s_nor.phase = TB_NOR_PHASE_WEL_POLL_WAIT;
            }
            break;

        case TB_NOR_PHASE_WEL_POLL_WAIT:
            if (Nor_ConsumeSpiResult())
            {
                s_nor.stats.status_register_1 = s_nor.rx[1];
                ++s_nor.stats.status_poll_count;
                if ((s_nor.rx[1] & TB_NOR_SR1_WEL) == 0U)
                {
                    ++s_nor.stats.write_enable_error_count;
                    Nor_Finish(TB_STATUS_HW_ERROR);
                }
                else
                {
                    s_nor.phase = TB_NOR_PHASE_COMMAND_LAUNCH;
                }
            }
            break;

        case TB_NOR_PHASE_COMMAND_LAUNCH:
            if (s_nor.operation == TB_NOR_OPERATION_ERASE)
            {
                Nor_PrepareAddressCommand(TB_NOR_CMD_SECTOR_ERASE);
                if (Nor_StartSpi(4U) == TB_STATUS_OK)
                {
                    s_nor.phase = TB_NOR_PHASE_COMMAND_WAIT;
                }
            }
            else if (s_nor.operation == TB_NOR_OPERATION_PROGRAM)
            {
                Nor_PrepareAddressCommand(TB_NOR_CMD_PAGE_PROGRAM);
                for (index = 0U; index < s_nor.data_length; ++index)
                {
                    s_nor.tx[index + 4U] = s_nor.program_data[index];
                }
                if (Nor_StartSpi((uint16_t)(s_nor.data_length + 4U)) == TB_STATUS_OK)
                {
                    s_nor.phase = TB_NOR_PHASE_COMMAND_WAIT;
                }
            }
            else
            {
                Nor_Finish(TB_STATUS_STATE_ERROR);
            }
            break;

        case TB_NOR_PHASE_COMMAND_WAIT:
            if (Nor_ConsumeSpiResult())
            {
                s_nor.deadline_ms = now_ms + s_nor.operation_timeout_ms;
                s_nor.next_poll_ms = now_ms;
                s_nor.phase = TB_NOR_PHASE_BUSY_POLL_LAUNCH;
            }
            break;

        case TB_NOR_PHASE_BUSY_POLL_LAUNCH:
            if (s_nor.deadline_ms == 0U)
            {
                s_nor.deadline_ms = now_ms + s_nor.operation_timeout_ms;
                s_nor.next_poll_ms = now_ms;
            }
            if (Nor_TimeReached(now_ms, s_nor.next_poll_ms))
            {
                s_nor.tx[0] = TB_NOR_CMD_READ_STATUS_1;
                s_nor.tx[1] = 0xFFU;
                if (Nor_StartSpi(2U) == TB_STATUS_OK)
                {
                    s_nor.phase = TB_NOR_PHASE_BUSY_POLL_WAIT;
                }
            }
            break;

        case TB_NOR_PHASE_BUSY_POLL_WAIT:
            if (Nor_ConsumeSpiResult())
            {
                s_nor.stats.status_register_1 = s_nor.rx[1];
                ++s_nor.stats.status_poll_count;
                if ((s_nor.rx[1] & TB_NOR_SR1_WIP) == 0U)
                {
                    if (s_nor.operation == TB_NOR_OPERATION_ERASE)
                    {
                        ++s_nor.stats.sector_erase_count;
                    }
                    else if (s_nor.operation == TB_NOR_OPERATION_PROGRAM)
                    {
                        ++s_nor.stats.page_program_count;
                        s_nor.stats.programmed_bytes += s_nor.data_length;
                    }
                    else if (s_nor.operation == TB_NOR_OPERATION_RECOVER)
                    {
                        ++s_nor.stats.recovery_count;
                    }
                    Nor_Finish(TB_STATUS_OK);
                }
                else
                {
                    s_nor.next_poll_ms = now_ms + TB_NOR_POLL_INTERVAL_MS;
                    s_nor.phase = TB_NOR_PHASE_BUSY_POLL_LAUNCH;
                }
            }
            break;

        case TB_NOR_PHASE_READ_LAUNCH:
            Nor_PrepareAddressCommand(TB_NOR_CMD_READ_DATA);
            for (index = 0U; index < s_nor.data_length; ++index)
            {
                s_nor.tx[index + 4U] = 0xFFU;
            }
            s_nor.deadline_ms = now_ms + s_nor.operation_timeout_ms;
            if (Nor_StartSpi((uint16_t)(s_nor.data_length + 4U)) == TB_STATUS_OK)
            {
                s_nor.phase = TB_NOR_PHASE_READ_WAIT;
            }
            break;

        case TB_NOR_PHASE_READ_WAIT:
            if (Nor_ConsumeSpiResult())
            {
                for (index = 0U; index < s_nor.data_length; ++index)
                {
                    s_nor.read_destination[index] = s_nor.rx[index + 4U];
                }
                ++s_nor.stats.read_count;
                s_nor.stats.read_bytes += s_nor.data_length;
                Nor_Finish(TB_STATUS_OK);
            }
            break;

        default:
            Nor_Finish(TB_STATUS_STATE_ERROR);
            break;
    }
}

void TbNor_GetStats(TbNorStats *stats_out)
{
    if (stats_out != 0)
    {
        *stats_out = s_nor.stats;
    }
}
