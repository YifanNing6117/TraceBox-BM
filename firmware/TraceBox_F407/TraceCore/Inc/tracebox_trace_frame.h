#ifndef TRACEBOX_TRACE_FRAME_H
#define TRACEBOX_TRACE_FRAME_H

#include <stdint.h>

#include "tracebox_types.h"

#define TB_TRACE_FRAME_SIZE             32U
#define TB_TRACE_FRAME_CRC_OFFSET       28U
#define TB_TRACE_FRAME_CRC_COVERAGE     28U
#define TB_TRACE_FRAME_PAYLOAD_CAPACITY 11U

#define TB_TRACE_SOURCE_TESTER  1U
#define TB_TRACE_SOURCE_ECU     2U

typedef enum
{
    TB_TRACE_FRAME_VALID = 0,
    TB_TRACE_FRAME_INVALID_MAGIC,
    TB_TRACE_FRAME_INVALID_VERSION,
    TB_TRACE_FRAME_INVALID_LENGTH,
    TB_TRACE_FRAME_INVALID_CRC
} TbTraceFrameValidation;

typedef struct
{
    uint8_t frame_type;
    uint8_t source_node;
    uint8_t severity;
    uint16_t event_id;
    uint32_t sequence;
    uint32_t timestamp_ms;
    uint8_t payload_length;
    uint8_t payload[TB_TRACE_FRAME_PAYLOAD_CAPACITY];
    uint32_t crc32;
} TbTraceFrame;

TbStatus TbTraceFrame_Decode(const uint8_t *raw_frame,
                             TbTraceFrame *frame_out,
                             TbTraceFrameValidation *validation_out);
uint32_t TbTraceFrame_ComputeCrc(const uint8_t *raw_frame);

#endif /* TRACEBOX_TRACE_FRAME_H */
