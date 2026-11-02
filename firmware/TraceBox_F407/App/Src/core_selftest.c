#include "core_selftest.h"

#include <stddef.h>

#include "tracebox_page_codec.h"

static uint16_t s_test_samples[TB_PAGE_SAMPLE_COUNT];
static uint8_t s_slot_indices[TB_PAGE_POOL_SLOT_COUNT];
static uint8_t *s_slot_pages[TB_PAGE_POOL_SLOT_COUNT];

#define SELFTEST_REQUIRE(expression, step_number) \
    do { if (!(expression)) { result_out->failure_step = (step_number); \
         result_out->status = TB_STATUS_VERIFY_FAILED; goto selftest_done; } } while (0)

TbStatus CoreSelfTest_Run(TbCoreSelfTestResult *result_out)
{
    TbAdcPageMetadata metadata;
    TbEventRecord event_record;
    TbPageValidation validation;
    TbStatus status;
    const uint8_t *ready_page;
    uint32_t calculated_crc;
    uint8_t slot_index;
    uint8_t index;

    if (result_out == NULL)
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    result_out->status = TB_STATUS_OK;
    result_out->failure_step = 0U;
    result_out->page_crc = 0U;
    result_out->crc_corruption_rejected = 0U;
    result_out->post_corruption_page_valid = 0U;

    TbEventRing_Init();
    SELFTEST_REQUIRE(
        TbEventRing_Push(0U, TB_EVENT_SEVERITY_INFO, 0U, 0U, 0U) ==
        TB_STATUS_INVALID_ARGUMENT, 1U);

    for (index = 0U; index < TB_EVENT_RING_CAPACITY; ++index)
    {
        status = TbEventRing_Push((uint16_t)(0x100U + index),
                                  (TbEventSeverity)(index % 5U), index,
                                  0xA5000000UL | index,
                                  (uint32_t)index * 10U);
        SELFTEST_REQUIRE(status == TB_STATUS_OK, 2U);
    }
    SELFTEST_REQUIRE(
        TbEventRing_Push(0x200U, TB_EVENT_SEVERITY_ERROR, 0U, 0U, 0U) ==
        TB_STATUS_OVERFLOW, 3U);

    for (index = 0U; index < TB_EVENT_RING_CAPACITY; ++index)
    {
        SELFTEST_REQUIRE(TbEventRing_Pop(&event_record) == TB_STATUS_OK, 4U);
        SELFTEST_REQUIRE(event_record.sequence == index, 5U);
        SELFTEST_REQUIRE(event_record.event_id == (uint16_t)(0x100U + index), 6U);
        SELFTEST_REQUIRE(event_record.value == (0xA5000000UL | index), 7U);
    }
    SELFTEST_REQUIRE(TbEventRing_Pop(&event_record) == TB_STATUS_BUSY, 8U);

    TbPagePool_Init();
    for (index = 0U; index < TB_PAGE_POOL_SLOT_COUNT; ++index)
    {
        SELFTEST_REQUIRE(
            TbPagePool_Acquire(&s_slot_indices[index], &s_slot_pages[index]) ==
            TB_STATUS_OK, 9U);
    }
    SELFTEST_REQUIRE(TbPagePool_Acquire(&slot_index, &s_slot_pages[0]) ==
                     TB_STATUS_OVERFLOW, 10U);

    for (index = 0U; index < TB_PAGE_SAMPLE_COUNT; ++index)
    {
        s_test_samples[index] = (uint16_t)(((uint16_t)index * 37U + 11U) & 0x0FFFU);
    }
    metadata.flags = 0xA5U;
    metadata.page_sequence = 0x01020304UL;
    metadata.t0_us = 0x10203040UL;
    metadata.sample_period_ns = 100000U;
    metadata.capture_id = 0x55667788UL;
    SELFTEST_REQUIRE(TbPage_BuildAdc(s_slot_pages[0], &metadata, s_test_samples,
                                    TB_PAGE_SAMPLE_COUNT,
                                    &result_out->page_crc) == TB_STATUS_OK, 11U);
    SELFTEST_REQUIRE(s_slot_pages[0][TB_PAGE_COMMIT_OFFSET] ==
                     TB_PAGE_COMMIT_ERASED, 12U);
    SELFTEST_REQUIRE(TbPage_BuildAdc(s_slot_pages[1], &metadata, s_test_samples,
                                    TB_PAGE_SAMPLE_COUNT - 1U,
                                    &calculated_crc) == TB_STATUS_RANGE_ERROR, 13U);

    for (index = 0U; index < TB_PAGE_POOL_SLOT_COUNT; ++index)
    {
        SELFTEST_REQUIRE(TbPagePool_Publish(s_slot_indices[index]) ==
                         TB_STATUS_OK, 14U);
    }

    for (index = 0U; index < TB_PAGE_POOL_SLOT_COUNT; ++index)
    {
        SELFTEST_REQUIRE(TbPagePool_TakeReady(&slot_index, &ready_page) ==
                         TB_STATUS_OK, 15U);
        SELFTEST_REQUIRE(slot_index == s_slot_indices[index], 16U);
        SELFTEST_REQUIRE(ready_page == s_slot_pages[index], 17U);

        if (index == 0U)
        {
            SELFTEST_REQUIRE(TbPage_MarkCommitted(s_slot_pages[0]) ==
                             TB_STATUS_OK, 18U);
            SELFTEST_REQUIRE(TbPage_Validate(ready_page, &validation,
                                             &calculated_crc) == TB_STATUS_OK,
                             19U);
            SELFTEST_REQUIRE(validation == TB_PAGE_VALID, 20U);

            s_slot_pages[0][TB_PAGE_HEADER_SIZE] ^= 0x01U;
            status = TbPage_Validate(ready_page, &validation, &calculated_crc);
            if ((status == TB_STATUS_CRC_ERROR) &&
                (validation == TB_PAGE_INVALID_CRC))
            {
                result_out->crc_corruption_rejected = 1U;
            }
            SELFTEST_REQUIRE(result_out->crc_corruption_rejected == 1U, 21U);

            s_slot_pages[0][TB_PAGE_HEADER_SIZE] ^= 0x01U;
            status = TbPage_Validate(ready_page, &validation, &calculated_crc);
            if ((status == TB_STATUS_OK) && (validation == TB_PAGE_VALID))
            {
                result_out->post_corruption_page_valid = 1U;
            }
            SELFTEST_REQUIRE(result_out->post_corruption_page_valid == 1U, 22U);
        }

        SELFTEST_REQUIRE(TbPagePool_Release(slot_index) == TB_STATUS_OK, 23U);
    }

    SELFTEST_REQUIRE(TbPagePool_TakeReady(&slot_index, &ready_page) ==
                     TB_STATUS_BUSY, 24U);

selftest_done:
    TbEventRing_GetStats(&result_out->event_stats);
    TbPagePool_GetStats(&result_out->pool_stats);
    return result_out->status;
}

#undef SELFTEST_REQUIRE
