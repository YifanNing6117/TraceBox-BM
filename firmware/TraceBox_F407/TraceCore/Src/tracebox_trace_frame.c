#include "tracebox_trace_frame.h"

#include <stddef.h>

#include "tracebox_crc.h"

#define TB_TRACE_FRAME_VERSION  1U

static uint16_t TraceFrame_ReadU16Le(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] |
                      ((uint16_t)source[1] << 8U));
}

static uint32_t TraceFrame_ReadU32Le(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

uint32_t TbTraceFrame_ComputeCrc(const uint8_t *raw_frame)
{
    uint32_t offset;

    if (raw_frame == NULL)
    {
        return 0U;
    }

    TbCrc_Begin();
    for (offset = 0U; offset < TB_TRACE_FRAME_CRC_COVERAGE; offset += 4U)
    {
        uint32_t word = ((uint32_t)raw_frame[offset] << 24U) |
                        ((uint32_t)raw_frame[offset + 1U] << 16U) |
                        ((uint32_t)raw_frame[offset + 2U] << 8U) |
                        (uint32_t)raw_frame[offset + 3U];
        TbCrc_WriteWord(word);
    }
    return TbCrc_Value();
}

TbStatus TbTraceFrame_Decode(const uint8_t *raw_frame,
                             TbTraceFrame *frame_out,
                             TbTraceFrameValidation *validation_out)
{
    uint32_t calculated_crc;
    uint32_t stored_crc;
    uint8_t index;

    if ((raw_frame == NULL) || (frame_out == NULL) ||
        (validation_out == NULL))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }
    if ((raw_frame[0] != 'T') || (raw_frame[1] != 'F'))
    {
        *validation_out = TB_TRACE_FRAME_INVALID_MAGIC;
        return TB_STATUS_FORMAT_ERROR;
    }
    if (raw_frame[2] != TB_TRACE_FRAME_VERSION)
    {
        *validation_out = TB_TRACE_FRAME_INVALID_VERSION;
        return TB_STATUS_FORMAT_ERROR;
    }
    if (raw_frame[16] > TB_TRACE_FRAME_PAYLOAD_CAPACITY)
    {
        *validation_out = TB_TRACE_FRAME_INVALID_LENGTH;
        return TB_STATUS_RANGE_ERROR;
    }

    stored_crc = TraceFrame_ReadU32Le(&raw_frame[TB_TRACE_FRAME_CRC_OFFSET]);
    calculated_crc = TbTraceFrame_ComputeCrc(raw_frame);
    if (stored_crc != calculated_crc)
    {
        *validation_out = TB_TRACE_FRAME_INVALID_CRC;
        return TB_STATUS_CRC_ERROR;
    }

    frame_out->frame_type = raw_frame[3];
    frame_out->source_node = raw_frame[4];
    frame_out->severity = raw_frame[5];
    frame_out->event_id = TraceFrame_ReadU16Le(&raw_frame[6]);
    frame_out->sequence = TraceFrame_ReadU32Le(&raw_frame[8]);
    frame_out->timestamp_ms = TraceFrame_ReadU32Le(&raw_frame[12]);
    frame_out->payload_length = raw_frame[16];
    for (index = 0U; index < TB_TRACE_FRAME_PAYLOAD_CAPACITY; ++index)
    {
        frame_out->payload[index] = raw_frame[17U + index];
    }
    frame_out->crc32 = stored_crc;
    *validation_out = TB_TRACE_FRAME_VALID;
    return TB_STATUS_OK;
}
