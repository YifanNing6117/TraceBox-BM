#ifndef TRACEBOX_EVENT_RING_H
#define TRACEBOX_EVENT_RING_H

#include <stdint.h>

#include "tracebox_types.h"

#define TB_EVENT_RING_CAPACITY 32U

typedef enum
{
    TB_EVENT_SEVERITY_DEBUG = 0,
    TB_EVENT_SEVERITY_INFO,
    TB_EVENT_SEVERITY_WARNING,
    TB_EVENT_SEVERITY_ERROR,
    TB_EVENT_SEVERITY_FATAL
} TbEventSeverity;

typedef struct
{
    uint32_t sequence;
    uint32_t timestamp_ms;
    uint16_t event_id;
    uint8_t severity;
    uint8_t flags;
    uint32_t value;
} TbEventRecord;

typedef struct
{
    uint32_t pushed;
    uint32_t popped;
    uint32_t dropped_newest;
    uint32_t next_sequence;
    uint8_t depth;
    uint8_t high_water;
} TbEventRingStats;

void TbEventRing_Init(void);
TbStatus TbEventRing_Push(uint16_t event_id, TbEventSeverity severity,
                          uint8_t flags, uint32_t value, uint32_t timestamp_ms);
TbStatus TbEventRing_Pop(TbEventRecord *record_out);
void TbEventRing_GetStats(TbEventRingStats *stats_out);

#endif /* TRACEBOX_EVENT_RING_H */
