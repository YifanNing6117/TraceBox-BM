#ifndef CORE_SELFTEST_H
#define CORE_SELFTEST_H

#include <stdint.h>

#include "tracebox_event_ring.h"
#include "tracebox_page_pool.h"
#include "tracebox_types.h"

typedef struct
{
    TbStatus status;
    uint32_t failure_step;
    uint32_t page_crc;
    uint8_t crc_corruption_rejected;
    uint8_t post_corruption_page_valid;
    TbEventRingStats event_stats;
    TbPagePoolStats pool_stats;
} TbCoreSelfTestResult;

TbStatus CoreSelfTest_Run(TbCoreSelfTestResult *result_out);

#endif /* CORE_SELFTEST_H */
