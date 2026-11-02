#ifndef TRACEBOX_PAGE_CODEC_H
#define TRACEBOX_PAGE_CODEC_H

#include <stdint.h>

#include "tracebox_page_pool.h"
#include "tracebox_types.h"

#define TB_PAGE_HEADER_SIZE       32U
#define TB_PAGE_SAMPLE_COUNT      96U
#define TB_PAGE_PAYLOAD_SIZE      192U
#define TB_PAGE_CRC_COVERAGE      224U
#define TB_PAGE_COMMIT_OFFSET     255U
#define TB_PAGE_COMMIT_ERASED     0xFFU
#define TB_PAGE_COMMIT_VALID      0x00U
#define TB_PAGE_RECORD_TYPE_ADC   1U
#define TB_PAGE_RECORD_TYPE_TRACE 2U
#define TB_PAGE_TRACE_COUNT       1U
#define TB_PAGE_TRACE_PAYLOAD_SIZE 32U

typedef struct
{
    uint8_t flags;
    uint32_t page_sequence;
    uint32_t t0_us;
    uint32_t sample_period_ns;
    uint32_t capture_id;
} TbAdcPageMetadata;

typedef struct
{
    uint8_t flags;
    uint32_t page_sequence;
    uint32_t timestamp_us;
    uint32_t capture_id;
} TbTracePageMetadata;

typedef enum
{
    TB_PAGE_VALID = 0,
    TB_PAGE_INVALID_MAGIC,
    TB_PAGE_INVALID_FORMAT,
    TB_PAGE_INVALID_COMMIT,
    TB_PAGE_INVALID_CRC
} TbPageValidation;

TbStatus TbPage_BuildAdc(uint8_t *page, const TbAdcPageMetadata *metadata,
                         const uint16_t *samples, uint16_t sample_count,
                         uint32_t *crc_out);
TbStatus TbPage_BuildTrace(uint8_t *page,
                           const TbTracePageMetadata *metadata,
                           const uint8_t *trace_frame,
                           uint16_t trace_frame_size,
                           uint32_t *crc_out);
TbStatus TbPage_MarkCommitted(uint8_t *page);
TbStatus TbPage_Validate(const uint8_t *page, TbPageValidation *validation_out,
                         uint32_t *calculated_crc_out);

#endif /* TRACEBOX_PAGE_CODEC_H */
