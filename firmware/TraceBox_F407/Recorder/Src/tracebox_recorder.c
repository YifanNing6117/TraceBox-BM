#include "tracebox_recorder.h"

#include <stdbool.h>
#include <stddef.h>

#include "tracebox_adc_dma.h"
#include "tracebox_board.h"
#include "tracebox_nor.h"
#include "tracebox_page_codec.h"
#include "tracebox_page_pool.h"
#include "tracebox_spi_ingress.h"
#include "tracebox_trace_frame.h"

#define TB_RECORDER_ERASE_TIMEOUT_MS    1000U
#define TB_RECORDER_PROGRAM_TIMEOUT_MS  100U
#define TB_RECORDER_BLOCK_CYCLES        1612800UL
#define TB_RECORDER_BLOCK_PERIOD_US     9600UL

typedef enum
{
    TB_RECORDER_NOR_NONE = 0,
    TB_RECORDER_NOR_IDENTIFY,
    TB_RECORDER_NOR_ERASE,
    TB_RECORDER_NOR_WRITE_BODY,
    TB_RECORDER_NOR_WRITE_COMMIT,
    TB_RECORDER_NOR_VERIFY_READ,
    TB_RECORDER_NOR_RECOVERY_READ
} TbRecorderNorPhase;

typedef struct
{
    TbRecorderStats stats;
    TbRecorderNorPhase nor_phase;
    uint32_t next_adc_sequence;
    uint32_t next_trace_sequence[3];
    bool trace_sequence_seen[3];
    uint32_t erase_index;
    uint32_t verify_index;
    uint32_t capture_started_at_cycles;
    uint32_t capture_started_at_ms;
    uint32_t now_ms;
    uint32_t scan_limit_pages;
    uint32_t recovery_capture_id;
    uint8_t writing_slot;
    const uint8_t *writing_page;
    bool writer_active;
    bool verify_after_record;
    bool recovery_has_capture;
    uint8_t verify_page[TB_NOR_PAGE_SIZE_BYTES];
    TbRecorderCompletionCallback callback;
    void *user_context;
} TbRecorderContext;

static TbRecorderContext s_recorder;

static void Recorder_NorComplete(TbStatus status, void *user_context);

static uint32_t Recorder_ReadU32Le(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static bool Recorder_PageIsErased(const uint8_t *page)
{
    uint16_t index;

    for (index = 0U; index < TB_NOR_PAGE_SIZE_BYTES; ++index)
    {
        if (page[index] != 0xFFU)
        {
            return false;
        }
    }
    return true;
}

static bool Recorder_StateIsBusy(TbRecorderState state)
{
    return (state == TB_RECORDER_STATE_PREPARING) ||
           (state == TB_RECORDER_STATE_RECORDING) ||
           (state == TB_RECORDER_STATE_DRAINING) ||
           (state == TB_RECORDER_STATE_VERIFYING) ||
           (state == TB_RECORDER_STATE_RECOVERING);
}

static void Recorder_UpdateSharedStats(void)
{
    TbAdcDmaStats adc_stats;
    TbPagePoolStats pool_stats;
    TbSpiIngressStats ingress_stats;

    TbAdcDma_GetStats(&adc_stats);
    TbPagePool_GetStats(&pool_stats);
    TbSpiIngress_GetStats(&ingress_stats);
    s_recorder.stats.page_pool_high_water = pool_stats.in_use_high_water;
    s_recorder.stats.page_pool_overflow = pool_stats.overflow_count;
    s_recorder.stats.adc_descriptor_high_water =
        adc_stats.descriptor_high_water;
    s_recorder.stats.adc_descriptor_overflow =
        adc_stats.descriptor_overflow_count;
    s_recorder.stats.adc_dma_errors = adc_stats.dma_error_count;
    s_recorder.stats.adc_overruns = adc_stats.adc_overrun_count;
    s_recorder.stats.ingress_descriptor_high_water =
        ingress_stats.descriptor_high_water;
    s_recorder.stats.ingress_descriptor_overflow =
        ingress_stats.descriptor_overflow_count;
    s_recorder.stats.ingress_dma_errors = ingress_stats.dma_error_count;
    s_recorder.stats.ingress_spi_errors = ingress_stats.spi_error_count;
    s_recorder.stats.ingress_short_frames = ingress_stats.short_frame_count;
    s_recorder.stats.ingress_recoveries = ingress_stats.recovery_count;
    s_recorder.stats.ingress_max_isr_cycles = ingress_stats.max_isr_cycles;
}

static void Recorder_ClearRunStats(void)
{
    s_recorder.stats.adc_blocks_seen = 0U;
    s_recorder.stats.pages_packed = 0U;
    s_recorder.stats.pages_committed = 0U;
    s_recorder.stats.pages_verified = 0U;
    s_recorder.stats.valid_pages = 0U;
    s_recorder.stats.recovery_pages_scanned = 0U;
    s_recorder.stats.rejected_torn_pages = 0U;
    s_recorder.stats.sequence_errors = 0U;
    s_recorder.stats.page_crc_errors = 0U;
    s_recorder.stats.flash_errors = 0U;
    s_recorder.stats.pack_last_cycles = 0U;
    s_recorder.stats.pack_max_cycles = 0U;
    s_recorder.stats.capture_cycles = 0U;
    s_recorder.stats.capture_ms = 0U;
    s_recorder.stats.page_pool_high_water = 0U;
    s_recorder.stats.page_pool_overflow = 0U;
    s_recorder.stats.adc_descriptor_high_water = 0U;
    s_recorder.stats.adc_descriptor_overflow = 0U;
    s_recorder.stats.adc_dma_errors = 0U;
    s_recorder.stats.adc_overruns = 0U;
    s_recorder.stats.trace_frames_seen = 0U;
    s_recorder.stats.trace_frames_accepted = 0U;
    s_recorder.stats.trace_frames_rejected = 0U;
    s_recorder.stats.trace_crc_errors = 0U;
    s_recorder.stats.trace_sequence_gaps = 0U;
    s_recorder.stats.ingress_descriptor_high_water = 0U;
    s_recorder.stats.ingress_descriptor_overflow = 0U;
    s_recorder.stats.ingress_dma_errors = 0U;
    s_recorder.stats.ingress_spi_errors = 0U;
    s_recorder.stats.ingress_short_frames = 0U;
    s_recorder.stats.ingress_recoveries = 0U;
    s_recorder.stats.ingress_max_isr_cycles = 0U;
}

static void Recorder_Finish(TbStatus status, TbRecorderState success_state)
{
    TbRecorderCompletionCallback callback = s_recorder.callback;
    void *user_context = s_recorder.user_context;

    if ((s_recorder.stats.state == TB_RECORDER_STATE_RECORDING) &&
        (s_recorder.stats.source == TB_RECORDER_SOURCE_ADC))
    {
        (void)TbAdcDma_Stop();
    }
    else if ((s_recorder.stats.state == TB_RECORDER_STATE_RECORDING) &&
             (s_recorder.stats.source == TB_RECORDER_SOURCE_TRACE))
    {
        (void)TbSpiIngress_Stop();
    }
    if (s_recorder.writer_active)
    {
        (void)TbPagePool_Release(s_recorder.writing_slot);
        s_recorder.writer_active = false;
    }

    Recorder_UpdateSharedStats();
    s_recorder.stats.last_status = (int32_t)status;
    s_recorder.stats.state = (status == TB_STATUS_OK) ?
                             success_state : TB_RECORDER_STATE_ERROR;
    s_recorder.stats.operation = TB_RECORDER_OPERATION_NONE;
    s_recorder.nor_phase = TB_RECORDER_NOR_NONE;
    s_recorder.callback = 0;
    s_recorder.user_context = 0;
    if (callback != 0)
    {
        callback(status, user_context);
    }
}

static bool Recorder_OperationStarted(TbStatus status)
{
    if (status == TB_STATUS_OK)
    {
        return true;
    }

    ++s_recorder.stats.flash_errors;
    Recorder_Finish(status, TB_RECORDER_STATE_COMPLETE);
    return false;
}

static bool Recorder_StartErase(uint32_t sector_index)
{
    uint32_t address = TB_RECORDER_DATA_BASE_ADDRESS +
                       (sector_index * TB_NOR_SECTOR_SIZE_BYTES);

    s_recorder.nor_phase = TB_RECORDER_NOR_ERASE;
    return Recorder_OperationStarted(TbNor_SectorEraseStart(
        address, TB_RECORDER_ERASE_TIMEOUT_MS, Recorder_NorComplete, 0));
}

static bool Recorder_StartRead(TbRecorderNorPhase phase, uint32_t page_index)
{
    uint32_t address = TB_RECORDER_DATA_BASE_ADDRESS +
                       (page_index * TB_NOR_PAGE_SIZE_BYTES);

    s_recorder.nor_phase = phase;
    return Recorder_OperationStarted(TbNor_ReadStart(
        address, s_recorder.verify_page, TB_NOR_PAGE_SIZE_BYTES,
        Recorder_NorComplete, 0));
}

static TbStatus Recorder_StartCapture(uint32_t capture_id,
                                      uint32_t target_pages,
                                      bool verify_after_record,
                                      TbRecorderSource source)
{
    TbStatus status;

    TbPagePool_Init();
    Recorder_ClearRunStats();
    s_recorder.stats.capture_id = capture_id;
    s_recorder.stats.target_pages = target_pages;
    s_recorder.stats.source = source;
    s_recorder.next_adc_sequence = 0U;
    s_recorder.next_trace_sequence[0] = 0U;
    s_recorder.next_trace_sequence[1] = 0U;
    s_recorder.next_trace_sequence[2] = 0U;
    s_recorder.trace_sequence_seen[0] = false;
    s_recorder.trace_sequence_seen[1] = false;
    s_recorder.trace_sequence_seen[2] = false;
    s_recorder.verify_index = 0U;
    s_recorder.writer_active = false;
    s_recorder.verify_after_record = verify_after_record;

    if (source == TB_RECORDER_SOURCE_ADC)
    {
        status = TbAdcDma_Start();
    }
    else if (source == TB_RECORDER_SOURCE_TRACE)
    {
        status = TbSpiIngress_Start();
    }
    else
    {
        status = TB_STATUS_INVALID_ARGUMENT;
    }
    if (status != TB_STATUS_OK)
    {
        return status;
    }

    s_recorder.capture_started_at_cycles = TbBoard_DwtCycles();
    s_recorder.capture_started_at_ms = s_recorder.now_ms;
    s_recorder.stats.state = TB_RECORDER_STATE_RECORDING;
    s_recorder.nor_phase = TB_RECORDER_NOR_NONE;
    return TB_STATUS_OK;
}

static bool Recorder_IdentifyMatchesCapacity(void)
{
    TbNorStats nor_stats;
    uint32_t bytes_required;

    TbNor_GetStats(&nor_stats);
    bytes_required = TB_RECORDER_DATA_BASE_ADDRESS +
                     (s_recorder.stats.sectors_required *
                      TB_NOR_SECTOR_SIZE_BYTES);
    return (nor_stats.manufacturer_id == 0xEFU) &&
           (nor_stats.capacity_code == 0x17U) &&
           (nor_stats.capacity_bytes >= bytes_required);
}

static void Recorder_CompleteRecoveryPage(void)
{
    TbPageValidation validation = TB_PAGE_INVALID_MAGIC;
    uint32_t calculated_crc = 0U;
    uint32_t sequence;
    uint32_t capture_id;
    TbStatus status;

    ++s_recorder.stats.recovery_pages_scanned;
    status = TbPage_Validate(s_recorder.verify_page, &validation,
                             &calculated_crc);
    if (status != TB_STATUS_OK)
    {
        if (!Recorder_PageIsErased(s_recorder.verify_page))
        {
            ++s_recorder.stats.rejected_torn_pages;
            if (validation == TB_PAGE_INVALID_CRC)
            {
                ++s_recorder.stats.page_crc_errors;
            }
        }
        Recorder_Finish(TB_STATUS_OK, TB_RECORDER_STATE_RECOVERED);
        return;
    }

    sequence = Recorder_ReadU32Le(&s_recorder.verify_page[8]);
    capture_id = Recorder_ReadU32Le(&s_recorder.verify_page[28]);
    if ((sequence != s_recorder.stats.valid_pages) ||
        (s_recorder.recovery_has_capture &&
         (capture_id != s_recorder.recovery_capture_id)))
    {
        ++s_recorder.stats.sequence_errors;
        ++s_recorder.stats.rejected_torn_pages;
        Recorder_Finish(TB_STATUS_OK, TB_RECORDER_STATE_RECOVERED);
        return;
    }

    if (!s_recorder.recovery_has_capture)
    {
        s_recorder.recovery_has_capture = true;
        s_recorder.recovery_capture_id = capture_id;
        s_recorder.stats.capture_id = capture_id;
    }
    ++s_recorder.stats.valid_pages;
    ++s_recorder.stats.pages_verified;
    ++s_recorder.verify_index;
    if (s_recorder.verify_index >= s_recorder.scan_limit_pages)
    {
        Recorder_Finish(TB_STATUS_OK, TB_RECORDER_STATE_RECOVERED);
    }
    else
    {
        (void)Recorder_StartRead(TB_RECORDER_NOR_RECOVERY_READ,
                                 s_recorder.verify_index);
    }
}

static void Recorder_NorComplete(TbStatus status, void *user_context)
{
    (void)user_context;
    if (s_recorder.stats.state == TB_RECORDER_STATE_ERROR)
    {
        return;
    }
    if (status != TB_STATUS_OK)
    {
        ++s_recorder.stats.flash_errors;
        Recorder_Finish(status, TB_RECORDER_STATE_COMPLETE);
        return;
    }

    switch (s_recorder.nor_phase)
    {
        case TB_RECORDER_NOR_IDENTIFY:
            if (!Recorder_IdentifyMatchesCapacity())
            {
                ++s_recorder.stats.flash_errors;
                Recorder_Finish(TB_STATUS_VERIFY_FAILED,
                                TB_RECORDER_STATE_COMPLETE);
            }
            else if (s_recorder.stats.operation ==
                     TB_RECORDER_OPERATION_RECOVER)
            {
                s_recorder.verify_index = 0U;
                (void)Recorder_StartRead(TB_RECORDER_NOR_RECOVERY_READ, 0U);
            }
            else
            {
                s_recorder.erase_index = 0U;
                (void)Recorder_StartErase(0U);
            }
            break;

        case TB_RECORDER_NOR_ERASE:
            ++s_recorder.stats.sectors_prepared;
            ++s_recorder.erase_index;
            if (s_recorder.erase_index < s_recorder.stats.sectors_required)
            {
                (void)Recorder_StartErase(s_recorder.erase_index);
            }
            else
            {
                s_recorder.stats.prepared_capacity_pages =
                    s_recorder.stats.sectors_required *
                    TB_RECORDER_PAGES_PER_SECTOR;
                if (s_recorder.stats.operation ==
                    TB_RECORDER_OPERATION_PREPARE)
                {
                    Recorder_Finish(TB_STATUS_OK,
                                    TB_RECORDER_STATE_PREPARED);
                }
                else if (Recorder_StartCapture(
                             s_recorder.stats.capture_id,
                             s_recorder.stats.target_pages, true,
                             TB_RECORDER_SOURCE_ADC) !=
                         TB_STATUS_OK)
                {
                    Recorder_Finish(TB_STATUS_HW_ERROR,
                                    TB_RECORDER_STATE_COMPLETE);
                }
                else
                {
                    s_recorder.stats.operation =
                        TB_RECORDER_OPERATION_SELFTEST;
                }
            }
            break;

        case TB_RECORDER_NOR_WRITE_BODY:
            s_recorder.nor_phase = TB_RECORDER_NOR_WRITE_COMMIT;
            (void)Recorder_OperationStarted(TbNor_PageProgramStart(
                TB_RECORDER_DATA_BASE_ADDRESS +
                    (s_recorder.stats.pages_committed *
                     TB_NOR_PAGE_SIZE_BYTES) + TB_PAGE_COMMIT_OFFSET,
                &s_recorder.writing_page[TB_PAGE_COMMIT_OFFSET], 1U,
                TB_RECORDER_PROGRAM_TIMEOUT_MS, Recorder_NorComplete, 0));
            break;

        case TB_RECORDER_NOR_WRITE_COMMIT:
            if (TbPagePool_Release(s_recorder.writing_slot) != TB_STATUS_OK)
            {
                ++s_recorder.stats.flash_errors;
                s_recorder.writer_active = false;
                Recorder_Finish(TB_STATUS_STATE_ERROR,
                                TB_RECORDER_STATE_COMPLETE);
                return;
            }
            s_recorder.writer_active = false;
            s_recorder.nor_phase = TB_RECORDER_NOR_NONE;
            ++s_recorder.stats.pages_committed;
            if ((s_recorder.stats.state == TB_RECORDER_STATE_RECORDING) &&
                (s_recorder.stats.source == TB_RECORDER_SOURCE_TRACE))
            {
                (void)TbSpiIngress_GrantCredit();
            }
            break;

        case TB_RECORDER_NOR_VERIFY_READ:
        {
            TbPageValidation validation;
            uint32_t calculated_crc;
            uint32_t sequence = Recorder_ReadU32Le(&s_recorder.verify_page[8]);

            if ((TbPage_Validate(s_recorder.verify_page, &validation,
                                 &calculated_crc) != TB_STATUS_OK) ||
                (validation != TB_PAGE_VALID))
            {
                ++s_recorder.stats.page_crc_errors;
                Recorder_Finish(TB_STATUS_CRC_ERROR,
                                TB_RECORDER_STATE_COMPLETE);
                return;
            }
            if ((sequence != s_recorder.verify_index) ||
                (Recorder_ReadU32Le(&s_recorder.verify_page[28]) !=
                 s_recorder.stats.capture_id))
            {
                ++s_recorder.stats.sequence_errors;
                Recorder_Finish(TB_STATUS_VERIFY_FAILED,
                                TB_RECORDER_STATE_COMPLETE);
                return;
            }

            ++s_recorder.stats.pages_verified;
            ++s_recorder.verify_index;
            if (s_recorder.verify_index < s_recorder.stats.target_pages)
            {
                (void)Recorder_StartRead(TB_RECORDER_NOR_VERIFY_READ,
                                         s_recorder.verify_index);
            }
            else
            {
                s_recorder.stats.valid_pages = s_recorder.stats.pages_verified;
                Recorder_Finish(TB_STATUS_OK, TB_RECORDER_STATE_COMPLETE);
            }
            break;
        }

        case TB_RECORDER_NOR_RECOVERY_READ:
            Recorder_CompleteRecoveryPage();
            break;

        default:
            ++s_recorder.stats.flash_errors;
            Recorder_Finish(TB_STATUS_STATE_ERROR,
                            TB_RECORDER_STATE_COMPLETE);
            break;
    }
}

static void Recorder_ServiceAdcProducer(uint32_t now_ms)
{
    TbAdcDmaDescriptor descriptor;

    while ((s_recorder.stats.state == TB_RECORDER_STATE_RECORDING) &&
           (TbAdcDma_PopDescriptor(&descriptor) == TB_STATUS_OK))
    {
        uint8_t slot_index;
        uint8_t *page;
        const uint16_t *samples;
        TbAdcPageMetadata metadata;
        uint32_t page_crc;
        uint32_t started_at;
        TbStatus status;

        ++s_recorder.stats.adc_blocks_seen;
        if (descriptor.sequence != s_recorder.next_adc_sequence)
        {
            ++s_recorder.stats.sequence_errors;
        }
        s_recorder.next_adc_sequence = descriptor.sequence + 1U;

        samples = TbAdcDma_GetBuffer(descriptor.buffer_index);
        status = TbPagePool_Acquire(&slot_index, &page);
        if ((status != TB_STATUS_OK) || (samples == 0))
        {
            Recorder_Finish((status == TB_STATUS_OK) ?
                            TB_STATUS_STATE_ERROR : status,
                            TB_RECORDER_STATE_COMPLETE);
            return;
        }

        metadata.flags = 0U;
        metadata.page_sequence = s_recorder.stats.pages_packed;
        metadata.t0_us = s_recorder.stats.pages_packed *
                         TB_RECORDER_BLOCK_PERIOD_US;
        metadata.sample_period_ns = 100000UL;
        metadata.capture_id = s_recorder.stats.capture_id;
        started_at = TbBoard_DwtCycles();
        status = TbPage_BuildAdc(page, &metadata, samples,
                                 descriptor.sample_count, &page_crc);
        if (status == TB_STATUS_OK)
        {
            status = TbPage_MarkCommitted(page);
        }
        s_recorder.stats.pack_last_cycles = TbBoard_DwtCycles() - started_at;
        if (s_recorder.stats.pack_last_cycles >
            s_recorder.stats.pack_max_cycles)
        {
            s_recorder.stats.pack_max_cycles =
                s_recorder.stats.pack_last_cycles;
        }
        if (status != TB_STATUS_OK)
        {
            (void)TbPagePool_Release(slot_index);
            Recorder_Finish(status, TB_RECORDER_STATE_COMPLETE);
            return;
        }
        if (TbPagePool_Publish(slot_index) != TB_STATUS_OK)
        {
            (void)TbPagePool_Release(slot_index);
            Recorder_Finish(TB_STATUS_STATE_ERROR,
                            TB_RECORDER_STATE_COMPLETE);
            return;
        }

        ++s_recorder.stats.pages_packed;
        if (s_recorder.stats.pages_packed >= s_recorder.stats.target_pages)
        {
            s_recorder.stats.capture_cycles =
                TbBoard_DwtCycles() - s_recorder.capture_started_at_cycles;
            s_recorder.stats.capture_ms =
                now_ms - s_recorder.capture_started_at_ms;
            (void)TbAdcDma_Stop();
            s_recorder.stats.state = TB_RECORDER_STATE_DRAINING;
        }
    }
}

static void Recorder_ServiceTraceProducer(uint32_t now_ms)
{
    TbSpiIngressDescriptor descriptor;

    while ((s_recorder.stats.state == TB_RECORDER_STATE_RECORDING) &&
           (TbSpiIngress_PopDescriptor(&descriptor) == TB_STATUS_OK))
    {
        const uint8_t *raw_frame =
            TbSpiIngress_GetBuffer(descriptor.buffer_index);
        TbTraceFrame frame;
        TbTraceFrameValidation validation;
        uint8_t slot_index = 0U;
        uint8_t *page = 0;
        uint32_t page_crc;
        uint32_t started_at;
        TbStatus status;

        ++s_recorder.stats.trace_frames_seen;
        status = (raw_frame == 0) ? TB_STATUS_STATE_ERROR :
                 TbTraceFrame_Decode(raw_frame, &frame, &validation);
        if ((status != TB_STATUS_OK) ||
            (frame.source_node < TB_TRACE_SOURCE_TESTER) ||
            (frame.source_node > TB_TRACE_SOURCE_ECU))
        {
            ++s_recorder.stats.trace_frames_rejected;
            if (status == TB_STATUS_CRC_ERROR)
            {
                ++s_recorder.stats.trace_crc_errors;
            }
            if (raw_frame != 0)
            {
                (void)TbSpiIngress_ReleaseBuffer(descriptor.buffer_index);
            }
            (void)TbSpiIngress_GrantCredit();
            continue;
        }

        if (s_recorder.trace_sequence_seen[frame.source_node] &&
            (frame.sequence !=
             s_recorder.next_trace_sequence[frame.source_node]))
        {
            ++s_recorder.stats.trace_sequence_gaps;
        }
        s_recorder.trace_sequence_seen[frame.source_node] = true;
        s_recorder.next_trace_sequence[frame.source_node] = frame.sequence + 1U;

        status = TbPagePool_Acquire(&slot_index, &page);
        if (status == TB_STATUS_OK)
        {
            TbTracePageMetadata metadata;

            metadata.flags = 0U;
            metadata.page_sequence = s_recorder.stats.pages_packed;
            metadata.timestamp_us = frame.timestamp_ms * 1000UL;
            metadata.capture_id = s_recorder.stats.capture_id;
            started_at = TbBoard_DwtCycles();
            status = TbPage_BuildTrace(page, &metadata, raw_frame,
                                       TB_TRACE_FRAME_SIZE, &page_crc);
            if (status == TB_STATUS_OK)
            {
                status = TbPage_MarkCommitted(page);
            }
            s_recorder.stats.pack_last_cycles =
                TbBoard_DwtCycles() - started_at;
            if (s_recorder.stats.pack_last_cycles >
                s_recorder.stats.pack_max_cycles)
            {
                s_recorder.stats.pack_max_cycles =
                    s_recorder.stats.pack_last_cycles;
            }
            if (status == TB_STATUS_OK)
            {
                status = TbPagePool_Publish(slot_index);
            }
        }

        (void)TbSpiIngress_ReleaseBuffer(descriptor.buffer_index);
        if (status != TB_STATUS_OK)
        {
            if (page != 0)
            {
                (void)TbPagePool_Release(slot_index);
            }
            Recorder_Finish(status, TB_RECORDER_STATE_COMPLETE);
            return;
        }

        ++s_recorder.stats.trace_frames_accepted;
        ++s_recorder.stats.pages_packed;
        if (s_recorder.stats.pages_packed >= s_recorder.stats.target_pages)
        {
            s_recorder.stats.capture_cycles =
                TbBoard_DwtCycles() - s_recorder.capture_started_at_cycles;
            s_recorder.stats.capture_ms =
                now_ms - s_recorder.capture_started_at_ms;
            (void)TbSpiIngress_Stop();
            s_recorder.stats.state = TB_RECORDER_STATE_DRAINING;
        }
    }
}

static void Recorder_ServiceWriter(void)
{
    if (((s_recorder.stats.state != TB_RECORDER_STATE_RECORDING) &&
         (s_recorder.stats.state != TB_RECORDER_STATE_DRAINING)) ||
        s_recorder.writer_active)
    {
        return;
    }

    if (TbPagePool_TakeReady(&s_recorder.writing_slot,
                             &s_recorder.writing_page) == TB_STATUS_OK)
    {
        s_recorder.writer_active = true;
        s_recorder.nor_phase = TB_RECORDER_NOR_WRITE_BODY;
        (void)Recorder_OperationStarted(TbNor_PageProgramStart(
            TB_RECORDER_DATA_BASE_ADDRESS +
                (s_recorder.stats.pages_committed * TB_NOR_PAGE_SIZE_BYTES),
            s_recorder.writing_page, TB_PAGE_COMMIT_OFFSET,
            TB_RECORDER_PROGRAM_TIMEOUT_MS, Recorder_NorComplete, 0));
    }
}

static TbStatus Recorder_BeginPrepare(TbRecorderOperation operation,
                                      uint32_t capture_id,
                                      uint32_t target_pages,
                                      TbRecorderCompletionCallback callback,
                                      void *user_context)
{
    if ((target_pages == 0U) || (target_pages > TB_RECORDER_MAX_PAGES))
    {
        return TB_STATUS_RANGE_ERROR;
    }
    if (Recorder_StateIsBusy(s_recorder.stats.state))
    {
        return TB_STATUS_BUSY;
    }

    TbPagePool_Init();
    Recorder_ClearRunStats();
    s_recorder.stats.operation = operation;
    s_recorder.stats.last_status = (int32_t)TB_STATUS_BUSY;
    s_recorder.stats.capture_id = capture_id;
    s_recorder.stats.target_pages = target_pages;
    s_recorder.stats.prepared_capacity_pages = 0U;
    s_recorder.stats.sectors_required =
        (target_pages + (TB_RECORDER_PAGES_PER_SECTOR - 1U)) /
        TB_RECORDER_PAGES_PER_SECTOR;
    s_recorder.stats.sectors_prepared = 0U;
    s_recorder.erase_index = 0U;
    s_recorder.writer_active = false;
    s_recorder.callback = callback;
    s_recorder.user_context = user_context;
    s_recorder.stats.state = TB_RECORDER_STATE_PREPARING;
    s_recorder.nor_phase = TB_RECORDER_NOR_IDENTIFY;
    return Recorder_OperationStarted(TbNor_IdentifyStart(
        Recorder_NorComplete, 0)) ? TB_STATUS_OK : TB_STATUS_HW_ERROR;
}

TbStatus TbRecorder_Init(void)
{
    s_recorder.stats.state = TB_RECORDER_STATE_IDLE;
    s_recorder.stats.operation = TB_RECORDER_OPERATION_NONE;
    s_recorder.stats.source = TB_RECORDER_SOURCE_NONE;
    s_recorder.stats.last_status = (int32_t)TB_STATUS_OK;
    s_recorder.stats.prepared_capacity_pages = 0U;
    s_recorder.stats.valid_pages = 0U;
    s_recorder.callback = 0;
    s_recorder.user_context = 0;
    s_recorder.writer_active = false;
    s_recorder.nor_phase = TB_RECORDER_NOR_NONE;
    s_recorder.now_ms = 0U;
    return TB_STATUS_OK;
}

TbStatus TbRecorder_Start(uint32_t capture_id,
                          uint32_t target_pages,
                          TbRecorderCompletionCallback callback,
                          void *user_context)
{
    return Recorder_BeginPrepare(TB_RECORDER_OPERATION_SELFTEST, capture_id,
                                 target_pages, callback, user_context);
}

TbStatus TbRecorder_Prepare(uint32_t target_pages,
                            TbRecorderCompletionCallback callback,
                            void *user_context)
{
    return Recorder_BeginPrepare(TB_RECORDER_OPERATION_PREPARE, 0U,
                                 target_pages, callback, user_context);
}

TbStatus TbRecorder_Record(uint32_t capture_id,
                           uint32_t target_pages,
                           bool verify_after_record,
                           TbRecorderCompletionCallback callback,
                           void *user_context)
{
    TbStatus status;

    if ((target_pages == 0U) || (target_pages > TB_RECORDER_MAX_PAGES))
    {
        return TB_STATUS_RANGE_ERROR;
    }
    if ((s_recorder.stats.state != TB_RECORDER_STATE_PREPARED) ||
        (target_pages > s_recorder.stats.prepared_capacity_pages))
    {
        return Recorder_StateIsBusy(s_recorder.stats.state) ?
               TB_STATUS_BUSY : TB_STATUS_STATE_ERROR;
    }

    s_recorder.stats.operation = TB_RECORDER_OPERATION_RECORD;
    s_recorder.stats.last_status = (int32_t)TB_STATUS_BUSY;
    s_recorder.callback = callback;
    s_recorder.user_context = user_context;
    status = Recorder_StartCapture(capture_id, target_pages,
                                   verify_after_record,
                                   TB_RECORDER_SOURCE_ADC);
    if (status != TB_STATUS_OK)
    {
        Recorder_Finish(status, TB_RECORDER_STATE_COMPLETE);
    }
    else
    {
        s_recorder.stats.operation = TB_RECORDER_OPERATION_RECORD;
    }
    return status;
}

TbStatus TbRecorder_RecordTrace(uint32_t capture_id,
                                uint32_t target_frames,
                                bool verify_after_record,
                                TbRecorderCompletionCallback callback,
                                void *user_context)
{
    TbStatus status;

    if ((target_frames == 0U) || (target_frames > TB_RECORDER_MAX_PAGES))
    {
        return TB_STATUS_RANGE_ERROR;
    }
    if ((s_recorder.stats.state != TB_RECORDER_STATE_PREPARED) ||
        (target_frames > s_recorder.stats.prepared_capacity_pages))
    {
        return Recorder_StateIsBusy(s_recorder.stats.state) ?
               TB_STATUS_BUSY : TB_STATUS_STATE_ERROR;
    }

    s_recorder.stats.operation = TB_RECORDER_OPERATION_RECORD;
    s_recorder.stats.last_status = (int32_t)TB_STATUS_BUSY;
    s_recorder.callback = callback;
    s_recorder.user_context = user_context;
    status = Recorder_StartCapture(capture_id, target_frames,
                                   verify_after_record,
                                   TB_RECORDER_SOURCE_TRACE);
    if (status != TB_STATUS_OK)
    {
        Recorder_Finish(status, TB_RECORDER_STATE_COMPLETE);
    }
    else
    {
        s_recorder.stats.operation = TB_RECORDER_OPERATION_RECORD;
    }
    return status;
}

TbStatus TbRecorder_Recover(uint32_t scan_limit_pages,
                            TbRecorderCompletionCallback callback,
                            void *user_context)
{
    if ((scan_limit_pages == 0U) ||
        (scan_limit_pages > TB_RECORDER_MAX_PAGES))
    {
        return TB_STATUS_RANGE_ERROR;
    }
    if (Recorder_StateIsBusy(s_recorder.stats.state))
    {
        return TB_STATUS_BUSY;
    }

    TbPagePool_Init();
    Recorder_ClearRunStats();
    s_recorder.stats.operation = TB_RECORDER_OPERATION_RECOVER;
    s_recorder.stats.last_status = (int32_t)TB_STATUS_BUSY;
    s_recorder.stats.state = TB_RECORDER_STATE_RECOVERING;
    s_recorder.stats.target_pages = scan_limit_pages;
    s_recorder.stats.sectors_required =
        (scan_limit_pages + (TB_RECORDER_PAGES_PER_SECTOR - 1U)) /
        TB_RECORDER_PAGES_PER_SECTOR;
    s_recorder.stats.sectors_prepared = 0U;
    s_recorder.stats.prepared_capacity_pages = 0U;
    s_recorder.scan_limit_pages = scan_limit_pages;
    s_recorder.verify_index = 0U;
    s_recorder.recovery_capture_id = 0U;
    s_recorder.recovery_has_capture = false;
    s_recorder.writer_active = false;
    s_recorder.callback = callback;
    s_recorder.user_context = user_context;
    s_recorder.nor_phase = TB_RECORDER_NOR_IDENTIFY;
    return Recorder_OperationStarted(TbNor_IdentifyStart(
        Recorder_NorComplete, 0)) ? TB_STATUS_OK : TB_STATUS_HW_ERROR;
}

void TbRecorder_MainFunction(uint32_t now_ms)
{
    TbPagePoolStats pool_stats;

    s_recorder.now_ms = now_ms;
    if (s_recorder.stats.state == TB_RECORDER_STATE_RECORDING)
    {
        if (s_recorder.stats.source == TB_RECORDER_SOURCE_ADC)
        {
            Recorder_ServiceAdcProducer(now_ms);
        }
        else if (s_recorder.stats.source == TB_RECORDER_SOURCE_TRACE)
        {
            Recorder_ServiceTraceProducer(now_ms);
        }
        else
        {
            Recorder_Finish(TB_STATUS_STATE_ERROR,
                            TB_RECORDER_STATE_COMPLETE);
        }
    }
    Recorder_ServiceWriter();

    if ((s_recorder.stats.state == TB_RECORDER_STATE_DRAINING) &&
        !s_recorder.writer_active &&
        (s_recorder.stats.pages_committed == s_recorder.stats.target_pages))
    {
        TbPagePool_GetStats(&pool_stats);
        if (pool_stats.ready_depth == 0U)
        {
            if (s_recorder.verify_after_record)
            {
                s_recorder.stats.state = TB_RECORDER_STATE_VERIFYING;
                s_recorder.verify_index = 0U;
                (void)Recorder_StartRead(TB_RECORDER_NOR_VERIFY_READ, 0U);
            }
            else
            {
                s_recorder.stats.valid_pages =
                    s_recorder.stats.pages_committed;
                Recorder_Finish(TB_STATUS_OK, TB_RECORDER_STATE_COMPLETE);
            }
        }
    }
}

void TbRecorder_GetStats(TbRecorderStats *stats_out)
{
    if (stats_out != 0)
    {
        Recorder_UpdateSharedStats();
        *stats_out = s_recorder.stats;
    }
}

bool TbRecorder_IsBusy(void)
{
    return Recorder_StateIsBusy(s_recorder.stats.state);
}

uint32_t TbRecorder_GetReadablePages(void)
{
    return s_recorder.stats.valid_pages;
}
