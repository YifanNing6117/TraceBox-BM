#ifndef TRACEBOX_RECORDER_H
#define TRACEBOX_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#include "tracebox_types.h"

#define TB_RECORDER_DATA_BASE_ADDRESS  0x00002000UL
#define TB_RECORDER_MAX_PAGES          10000U
#define TB_RECORDER_PAGES_PER_SECTOR   16U

typedef enum
{
    TB_RECORDER_STATE_IDLE = 0,
    TB_RECORDER_STATE_PREPARING = 1,
    TB_RECORDER_STATE_RECORDING = 2,
    TB_RECORDER_STATE_DRAINING = 3,
    TB_RECORDER_STATE_VERIFYING = 4,
    TB_RECORDER_STATE_COMPLETE = 5,
    TB_RECORDER_STATE_ERROR = 6,
    TB_RECORDER_STATE_PREPARED = 7,
    TB_RECORDER_STATE_RECOVERING = 8,
    TB_RECORDER_STATE_RECOVERED = 9
} TbRecorderState;

typedef enum
{
    TB_RECORDER_OPERATION_NONE = 0,
    TB_RECORDER_OPERATION_PREPARE,
    TB_RECORDER_OPERATION_RECORD,
    TB_RECORDER_OPERATION_RECOVER,
    TB_RECORDER_OPERATION_SELFTEST
} TbRecorderOperation;

typedef enum
{
    TB_RECORDER_SOURCE_NONE = 0,
    TB_RECORDER_SOURCE_ADC = 1,
    TB_RECORDER_SOURCE_TRACE = 2
} TbRecorderSource;

typedef void (*TbRecorderCompletionCallback)(TbStatus status,
                                             void *user_context);

typedef struct
{
    TbRecorderState state;
    TbRecorderOperation operation;
    TbRecorderSource source;
    int32_t last_status;
    uint32_t capture_id;
    uint32_t target_pages;
    uint32_t prepared_capacity_pages;
    uint32_t sectors_required;
    uint32_t sectors_prepared;
    uint32_t adc_blocks_seen;
    uint32_t pages_packed;
    uint32_t pages_committed;
    uint32_t pages_verified;
    uint32_t valid_pages;
    uint32_t recovery_pages_scanned;
    uint32_t rejected_torn_pages;
    uint32_t sequence_errors;
    uint32_t page_crc_errors;
    uint32_t flash_errors;
    uint32_t pack_last_cycles;
    uint32_t pack_max_cycles;
    uint32_t capture_cycles;
    uint32_t capture_ms;
    uint32_t page_pool_high_water;
    uint32_t page_pool_overflow;
    uint32_t adc_descriptor_high_water;
    uint32_t adc_descriptor_overflow;
    uint32_t adc_dma_errors;
    uint32_t adc_overruns;
    uint32_t trace_frames_seen;
    uint32_t trace_frames_accepted;
    uint32_t trace_frames_rejected;
    uint32_t trace_crc_errors;
    uint32_t trace_sequence_gaps;
    uint32_t ingress_descriptor_high_water;
    uint32_t ingress_descriptor_overflow;
    uint32_t ingress_dma_errors;
    uint32_t ingress_spi_errors;
    uint32_t ingress_short_frames;
    uint32_t ingress_recoveries;
    uint32_t ingress_max_isr_cycles;
} TbRecorderStats;

TbStatus TbRecorder_Init(void);

/* Compatibility gate: identify, erase, record and verify in one operation. */
TbStatus TbRecorder_Start(uint32_t capture_id,
                          uint32_t target_pages,
                          TbRecorderCompletionCallback callback,
                          void *user_context);

TbStatus TbRecorder_Prepare(uint32_t target_pages,
                            TbRecorderCompletionCallback callback,
                            void *user_context);
TbStatus TbRecorder_Record(uint32_t capture_id,
                           uint32_t target_pages,
                           bool verify_after_record,
                           TbRecorderCompletionCallback callback,
                           void *user_context);
TbStatus TbRecorder_RecordTrace(uint32_t capture_id,
                                uint32_t target_frames,
                                bool verify_after_record,
                                TbRecorderCompletionCallback callback,
                                void *user_context);
TbStatus TbRecorder_Recover(uint32_t scan_limit_pages,
                            TbRecorderCompletionCallback callback,
                            void *user_context);

void TbRecorder_MainFunction(uint32_t now_ms);
void TbRecorder_GetStats(TbRecorderStats *stats_out);
bool TbRecorder_IsBusy(void);
uint32_t TbRecorder_GetReadablePages(void);

#endif /* TRACEBOX_RECORDER_H */
