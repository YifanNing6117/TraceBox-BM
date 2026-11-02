#include "tracebox_event_ring.h"

#include <stddef.h>

typedef char TbEventRecordSizeMustBe16Bytes[(sizeof(TbEventRecord) == 16U) ? 1 : -1];

#define TB_EVENT_RING_MASK (TB_EVENT_RING_CAPACITY - 1U)

typedef struct
{
    TbEventRecord records[TB_EVENT_RING_CAPACITY];
    TbEventRingStats stats;
    uint8_t head;
    uint8_t tail;
} TbEventRingContext;

static TbEventRingContext s_event_ring;

void TbEventRing_Init(void)
{
    s_event_ring.stats.pushed = 0U;
    s_event_ring.stats.popped = 0U;
    s_event_ring.stats.dropped_newest = 0U;
    s_event_ring.stats.next_sequence = 0U;
    s_event_ring.stats.depth = 0U;
    s_event_ring.stats.high_water = 0U;
    s_event_ring.head = 0U;
    s_event_ring.tail = 0U;
}

TbStatus TbEventRing_Push(uint16_t event_id, TbEventSeverity severity,
                          uint8_t flags, uint32_t value, uint32_t timestamp_ms)
{
    TbEventRecord *record;

    if ((event_id == 0U) || (severity > TB_EVENT_SEVERITY_FATAL))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    if (s_event_ring.stats.depth >= TB_EVENT_RING_CAPACITY)
    {
        ++s_event_ring.stats.dropped_newest;
        return TB_STATUS_OVERFLOW;
    }

    record = &s_event_ring.records[s_event_ring.head];
    record->sequence = s_event_ring.stats.next_sequence++;
    record->timestamp_ms = timestamp_ms;
    record->event_id = event_id;
    record->severity = (uint8_t)severity;
    record->flags = flags;
    record->value = value;

    s_event_ring.head = (uint8_t)
        ((s_event_ring.head + 1U) & TB_EVENT_RING_MASK);
    ++s_event_ring.stats.depth;
    ++s_event_ring.stats.pushed;
    if (s_event_ring.stats.depth > s_event_ring.stats.high_water)
    {
        s_event_ring.stats.high_water = s_event_ring.stats.depth;
    }

    return TB_STATUS_OK;
}

TbStatus TbEventRing_Pop(TbEventRecord *record_out)
{
    if (record_out == NULL)
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    if (s_event_ring.stats.depth == 0U)
    {
        return TB_STATUS_BUSY;
    }

    *record_out = s_event_ring.records[s_event_ring.tail];
    s_event_ring.tail = (uint8_t)
        ((s_event_ring.tail + 1U) & TB_EVENT_RING_MASK);
    --s_event_ring.stats.depth;
    ++s_event_ring.stats.popped;
    return TB_STATUS_OK;
}

void TbEventRing_GetStats(TbEventRingStats *stats_out)
{
    if (stats_out != NULL)
    {
        *stats_out = s_event_ring.stats;
    }
}
