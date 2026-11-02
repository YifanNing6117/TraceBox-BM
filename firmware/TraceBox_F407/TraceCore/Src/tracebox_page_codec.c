#include "tracebox_page_codec.h"

#include <stddef.h>

#include "tracebox_crc.h"

#define TB_PAGE_FORMAT_VERSION    1U
#define TB_PAGE_CRC_OFFSET        24U

static void Page_WriteU16Le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void Page_WriteU32Le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static uint16_t Page_ReadU16Le(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] |
                      ((uint16_t)source[1] << 8U));
}

static uint32_t Page_ReadU32Le(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static uint32_t Page_ComputeCrc(const uint8_t *page)
{
    uint32_t offset;

    TbCrc_Begin();
    for (offset = 0U; offset < TB_PAGE_CRC_COVERAGE; offset += 4U)
    {
        uint32_t word;

        if (offset == TB_PAGE_CRC_OFFSET)
        {
            word = 0U;
        }
        else
        {
            word = ((uint32_t)page[offset] << 24U) |
                   ((uint32_t)page[offset + 1U] << 16U) |
                   ((uint32_t)page[offset + 2U] << 8U) |
                   (uint32_t)page[offset + 3U];
        }
        TbCrc_WriteWord(word);
    }

    return TbCrc_Value();
}

TbStatus TbPage_BuildAdc(uint8_t *page, const TbAdcPageMetadata *metadata,
                         const uint16_t *samples, uint16_t sample_count,
                         uint32_t *crc_out)
{
    uint32_t crc;
    uint16_t index;

    if ((page == NULL) || (metadata == NULL) || (samples == NULL) ||
        (crc_out == NULL))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }
    if ((sample_count != TB_PAGE_SAMPLE_COUNT) ||
        (metadata->sample_period_ns == 0U))
    {
        return TB_STATUS_RANGE_ERROR;
    }

    for (index = 0U; index < TB_PAGE_SIZE_BYTES; ++index)
    {
        page[index] = 0xFFU;
    }

    page[0] = 'T';
    page[1] = 'B';
    page[2] = 'P';
    page[3] = '1';
    page[4] = TB_PAGE_FORMAT_VERSION;
    page[5] = TB_PAGE_RECORD_TYPE_ADC;
    page[6] = TB_PAGE_HEADER_SIZE;
    page[7] = metadata->flags;
    Page_WriteU32Le(&page[8], metadata->page_sequence);
    Page_WriteU32Le(&page[12], metadata->t0_us);
    Page_WriteU32Le(&page[16], metadata->sample_period_ns);
    Page_WriteU16Le(&page[20], TB_PAGE_SAMPLE_COUNT);
    Page_WriteU16Le(&page[22], TB_PAGE_PAYLOAD_SIZE);
    Page_WriteU32Le(&page[TB_PAGE_CRC_OFFSET], 0U);
    Page_WriteU32Le(&page[28], metadata->capture_id);

    for (index = 0U; index < TB_PAGE_SAMPLE_COUNT; ++index)
    {
        Page_WriteU16Le(&page[TB_PAGE_HEADER_SIZE + ((uint32_t)index * 2U)],
                        samples[index]);
    }

    crc = Page_ComputeCrc(page);
    Page_WriteU32Le(&page[TB_PAGE_CRC_OFFSET], crc);
    *crc_out = crc;
    return TB_STATUS_OK;
}

TbStatus TbPage_BuildTrace(uint8_t *page,
                           const TbTracePageMetadata *metadata,
                           const uint8_t *trace_frame,
                           uint16_t trace_frame_size,
                           uint32_t *crc_out)
{
    uint32_t crc;
    uint16_t index;

    if ((page == NULL) || (metadata == NULL) || (trace_frame == NULL) ||
        (crc_out == NULL))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }
    if (trace_frame_size != TB_PAGE_TRACE_PAYLOAD_SIZE)
    {
        return TB_STATUS_RANGE_ERROR;
    }

    for (index = 0U; index < TB_PAGE_SIZE_BYTES; ++index)
    {
        page[index] = 0xFFU;
    }

    page[0] = 'T';
    page[1] = 'B';
    page[2] = 'P';
    page[3] = '1';
    page[4] = TB_PAGE_FORMAT_VERSION;
    page[5] = TB_PAGE_RECORD_TYPE_TRACE;
    page[6] = TB_PAGE_HEADER_SIZE;
    page[7] = metadata->flags;
    Page_WriteU32Le(&page[8], metadata->page_sequence);
    Page_WriteU32Le(&page[12], metadata->timestamp_us);
    Page_WriteU32Le(&page[16], 0U);
    Page_WriteU16Le(&page[20], TB_PAGE_TRACE_COUNT);
    Page_WriteU16Le(&page[22], TB_PAGE_TRACE_PAYLOAD_SIZE);
    Page_WriteU32Le(&page[TB_PAGE_CRC_OFFSET], 0U);
    Page_WriteU32Le(&page[28], metadata->capture_id);
    for (index = 0U; index < trace_frame_size; ++index)
    {
        page[TB_PAGE_HEADER_SIZE + index] = trace_frame[index];
    }

    crc = Page_ComputeCrc(page);
    Page_WriteU32Le(&page[TB_PAGE_CRC_OFFSET], crc);
    *crc_out = crc;
    return TB_STATUS_OK;
}

TbStatus TbPage_MarkCommitted(uint8_t *page)
{
    if (page == NULL)
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }
    if (page[TB_PAGE_COMMIT_OFFSET] != TB_PAGE_COMMIT_ERASED)
    {
        return TB_STATUS_STATE_ERROR;
    }

    page[TB_PAGE_COMMIT_OFFSET] = TB_PAGE_COMMIT_VALID;
    return TB_STATUS_OK;
}

TbStatus TbPage_Validate(const uint8_t *page, TbPageValidation *validation_out,
                         uint32_t *calculated_crc_out)
{
    uint32_t stored_crc;
    uint32_t calculated_crc;

    if ((page == NULL) || (validation_out == NULL) ||
        (calculated_crc_out == NULL))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    if ((page[0] != 'T') || (page[1] != 'B') ||
        (page[2] != 'P') || (page[3] != '1'))
    {
        *validation_out = TB_PAGE_INVALID_MAGIC;
        return TB_STATUS_FORMAT_ERROR;
    }

    if ((page[4] != TB_PAGE_FORMAT_VERSION) ||
        (page[6] != TB_PAGE_HEADER_SIZE))
    {
        *validation_out = TB_PAGE_INVALID_FORMAT;
        return TB_STATUS_FORMAT_ERROR;
    }
    if (page[5] == TB_PAGE_RECORD_TYPE_ADC)
    {
        if ((Page_ReadU16Le(&page[20]) != TB_PAGE_SAMPLE_COUNT) ||
            (Page_ReadU16Le(&page[22]) != TB_PAGE_PAYLOAD_SIZE))
        {
            *validation_out = TB_PAGE_INVALID_FORMAT;
            return TB_STATUS_FORMAT_ERROR;
        }
    }
    else if (page[5] == TB_PAGE_RECORD_TYPE_TRACE)
    {
        if ((Page_ReadU16Le(&page[20]) != TB_PAGE_TRACE_COUNT) ||
            (Page_ReadU16Le(&page[22]) != TB_PAGE_TRACE_PAYLOAD_SIZE))
        {
            *validation_out = TB_PAGE_INVALID_FORMAT;
            return TB_STATUS_FORMAT_ERROR;
        }
    }
    else
    {
        *validation_out = TB_PAGE_INVALID_FORMAT;
        return TB_STATUS_FORMAT_ERROR;
    }

    if (page[TB_PAGE_COMMIT_OFFSET] != TB_PAGE_COMMIT_VALID)
    {
        *validation_out = TB_PAGE_INVALID_COMMIT;
        return TB_STATUS_STATE_ERROR;
    }

    stored_crc = Page_ReadU32Le(&page[TB_PAGE_CRC_OFFSET]);
    calculated_crc = Page_ComputeCrc(page);
    *calculated_crc_out = calculated_crc;
    if (stored_crc != calculated_crc)
    {
        *validation_out = TB_PAGE_INVALID_CRC;
        return TB_STATUS_CRC_ERROR;
    }

    *validation_out = TB_PAGE_VALID;
    return TB_STATUS_OK;
}
