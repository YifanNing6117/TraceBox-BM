#include "tracebox_page_pool.h"

#include <stddef.h>

#define TB_PAGE_POOL_SLOT_MASK (TB_PAGE_POOL_SLOT_COUNT - 1U)
#define TB_PAGE_WORD_COUNT     (TB_PAGE_SIZE_BYTES / sizeof(uint32_t))

typedef struct
{
    uint32_t pages[TB_PAGE_POOL_SLOT_COUNT][TB_PAGE_WORD_COUNT];
    TbPageSlotState states[TB_PAGE_POOL_SLOT_COUNT];
    uint8_t ready_queue[TB_PAGE_POOL_SLOT_COUNT];
    uint8_t ready_head;
    uint8_t ready_tail;
    TbPagePoolStats stats;
} TbPagePoolContext;

static TbPagePoolContext s_page_pool;

void TbPagePool_Init(void)
{
    uint8_t index;

    for (index = 0U; index < TB_PAGE_POOL_SLOT_COUNT; ++index)
    {
        s_page_pool.states[index] = TB_PAGE_SLOT_FREE;
        s_page_pool.ready_queue[index] = 0U;
    }

    s_page_pool.ready_head = 0U;
    s_page_pool.ready_tail = 0U;
    s_page_pool.stats.acquired = 0U;
    s_page_pool.stats.published = 0U;
    s_page_pool.stats.taken = 0U;
    s_page_pool.stats.released = 0U;
    s_page_pool.stats.overflow_count = 0U;
    s_page_pool.stats.ownership_error_count = 0U;
    s_page_pool.stats.in_use = 0U;
    s_page_pool.stats.in_use_high_water = 0U;
    s_page_pool.stats.ready_depth = 0U;
    s_page_pool.stats.ready_high_water = 0U;
}

TbStatus TbPagePool_Acquire(uint8_t *slot_index_out, uint8_t **page_out)
{
    uint8_t index;

    if ((slot_index_out == NULL) || (page_out == NULL))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0U; index < TB_PAGE_POOL_SLOT_COUNT; ++index)
    {
        if (s_page_pool.states[index] == TB_PAGE_SLOT_FREE)
        {
            s_page_pool.states[index] = TB_PAGE_SLOT_FILLING;
            *slot_index_out = index;
            *page_out = (uint8_t *)s_page_pool.pages[index];
            ++s_page_pool.stats.acquired;
            ++s_page_pool.stats.in_use;
            if (s_page_pool.stats.in_use > s_page_pool.stats.in_use_high_water)
            {
                s_page_pool.stats.in_use_high_water = s_page_pool.stats.in_use;
            }
            return TB_STATUS_OK;
        }
    }

    ++s_page_pool.stats.overflow_count;
    return TB_STATUS_OVERFLOW;
}

TbStatus TbPagePool_Publish(uint8_t slot_index)
{
    if (slot_index >= TB_PAGE_POOL_SLOT_COUNT)
    {
        ++s_page_pool.stats.ownership_error_count;
        return TB_STATUS_RANGE_ERROR;
    }

    if ((s_page_pool.states[slot_index] != TB_PAGE_SLOT_FILLING) ||
        (s_page_pool.stats.ready_depth >= TB_PAGE_POOL_SLOT_COUNT))
    {
        ++s_page_pool.stats.ownership_error_count;
        return TB_STATUS_STATE_ERROR;
    }

    s_page_pool.states[slot_index] = TB_PAGE_SLOT_READY;
    s_page_pool.ready_queue[s_page_pool.ready_head] = slot_index;
    s_page_pool.ready_head = (uint8_t)
        ((s_page_pool.ready_head + 1U) & TB_PAGE_POOL_SLOT_MASK);
    ++s_page_pool.stats.ready_depth;
    ++s_page_pool.stats.published;
    if (s_page_pool.stats.ready_depth > s_page_pool.stats.ready_high_water)
    {
        s_page_pool.stats.ready_high_water = s_page_pool.stats.ready_depth;
    }
    return TB_STATUS_OK;
}

TbStatus TbPagePool_TakeReady(uint8_t *slot_index_out, const uint8_t **page_out)
{
    uint8_t slot_index;

    if ((slot_index_out == NULL) || (page_out == NULL))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    if (s_page_pool.stats.ready_depth == 0U)
    {
        return TB_STATUS_BUSY;
    }

    slot_index = s_page_pool.ready_queue[s_page_pool.ready_tail];
    if ((slot_index >= TB_PAGE_POOL_SLOT_COUNT) ||
        (s_page_pool.states[slot_index] != TB_PAGE_SLOT_READY))
    {
        ++s_page_pool.stats.ownership_error_count;
        return TB_STATUS_STATE_ERROR;
    }

    s_page_pool.ready_tail = (uint8_t)
        ((s_page_pool.ready_tail + 1U) & TB_PAGE_POOL_SLOT_MASK);
    --s_page_pool.stats.ready_depth;
    s_page_pool.states[slot_index] = TB_PAGE_SLOT_WRITING;
    *slot_index_out = slot_index;
    *page_out = (const uint8_t *)s_page_pool.pages[slot_index];
    ++s_page_pool.stats.taken;
    return TB_STATUS_OK;
}

TbStatus TbPagePool_Release(uint8_t slot_index)
{
    if (slot_index >= TB_PAGE_POOL_SLOT_COUNT)
    {
        ++s_page_pool.stats.ownership_error_count;
        return TB_STATUS_RANGE_ERROR;
    }

    if ((s_page_pool.states[slot_index] != TB_PAGE_SLOT_FILLING) &&
        (s_page_pool.states[slot_index] != TB_PAGE_SLOT_WRITING))
    {
        ++s_page_pool.stats.ownership_error_count;
        return TB_STATUS_STATE_ERROR;
    }

    s_page_pool.states[slot_index] = TB_PAGE_SLOT_FREE;
    --s_page_pool.stats.in_use;
    ++s_page_pool.stats.released;
    return TB_STATUS_OK;
}

void TbPagePool_GetStats(TbPagePoolStats *stats_out)
{
    if (stats_out != NULL)
    {
        *stats_out = s_page_pool.stats;
    }
}
