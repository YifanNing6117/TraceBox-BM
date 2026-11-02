#ifndef TRACEBOX_PAGE_POOL_H
#define TRACEBOX_PAGE_POOL_H

#include <stdint.h>

#include "tracebox_types.h"

#define TB_PAGE_POOL_SLOT_COUNT 8U
#define TB_PAGE_SIZE_BYTES      256U

typedef enum
{
    TB_PAGE_SLOT_FREE = 0,
    TB_PAGE_SLOT_FILLING,
    TB_PAGE_SLOT_READY,
    TB_PAGE_SLOT_WRITING
} TbPageSlotState;

typedef struct
{
    uint32_t acquired;
    uint32_t published;
    uint32_t taken;
    uint32_t released;
    uint32_t overflow_count;
    uint32_t ownership_error_count;
    uint8_t in_use;
    uint8_t in_use_high_water;
    uint8_t ready_depth;
    uint8_t ready_high_water;
} TbPagePoolStats;

void TbPagePool_Init(void);
TbStatus TbPagePool_Acquire(uint8_t *slot_index_out, uint8_t **page_out);
TbStatus TbPagePool_Publish(uint8_t slot_index);
TbStatus TbPagePool_TakeReady(uint8_t *slot_index_out, const uint8_t **page_out);
TbStatus TbPagePool_Release(uint8_t slot_index);
void TbPagePool_GetStats(TbPagePoolStats *stats_out);

#endif /* TRACEBOX_PAGE_POOL_H */
