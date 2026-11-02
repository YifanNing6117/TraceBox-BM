#ifndef TRACE_LINK_H
#define TRACE_LINK_H

#include <stdint.h>

#define TRACE_LINK_FRAME_SIZE       32u
#define TRACE_LINK_PAYLOAD_CAPACITY 11u

#define TRACE_SOURCE_TESTER 1u
#define TRACE_SOURCE_ECU    2u

#define TRACE_SEVERITY_INFO  1u
#define TRACE_SEVERITY_WARN  2u
#define TRACE_SEVERITY_ERROR 3u

#define TRACE_EVENT_BOOT              0x0001u
#define TRACE_EVENT_CAN_TX            0x1001u
#define TRACE_EVENT_CAN_RX            0x1002u
#define TRACE_EVENT_CAN_ERROR         0x3001u
#define TRACE_EVENT_TX_QUEUE_OVERFLOW 0x4001u
#define TRACE_EVENT_RX_QUEUE_OVERFLOW 0x4002u

typedef enum {
    TRACE_LINK_OK = 0,
    TRACE_LINK_ERR_ARG = -1,
    TRACE_LINK_ERR_BUSY = -2,
    TRACE_LINK_ERR_FULL = -3,
    TRACE_LINK_ERR_HW = -4,
    TRACE_LINK_ERR_TIMEOUT = -5
} TraceLinkStatus;

typedef enum {
    TRACE_LINK_STATE_RESET = 0,
    TRACE_LINK_STATE_IDLE,
    TRACE_LINK_STATE_DMA_ACTIVE,
    TRACE_LINK_STATE_RECOVERING,
    TRACE_LINK_STATE_FAULT
} TraceLinkState;

typedef struct {
    TraceLinkState state;
    uint8_t ready_input;
    uint8_t queue_pending;
    uint8_t queue_high_water;
    uint32_t frames_enqueued;
    uint32_t frames_sent;
    uint32_t queue_rejected;
    uint32_t dma_irq_count;
    uint32_t dma_error_count;
    uint32_t timeout_count;
    uint32_t recovery_count;
    uint32_t backpressure_count;
    uint32_t bad_crc_injected;
} TraceLinkStats;

TraceLinkStatus TraceLink_Init(void);
TraceLinkStatus TraceLink_Emit(uint8_t source_node,
                               uint16_t event_id,
                               uint8_t severity,
                               const uint8_t *payload,
                               uint8_t payload_length,
                               uint32_t timestamp_ms);
TraceLinkStatus TraceLink_EmitCan(uint8_t source_node,
                                  uint16_t event_id,
                                  uint8_t severity,
                                  uint16_t can_id,
                                  uint8_t dlc,
                                  const uint8_t *data,
                                  uint32_t timestamp_ms);
void TraceLink_InjectBadCrcOnce(void);
void TraceLink_MainFunction(uint32_t now_ms);
void TraceLink_GetStats(TraceLinkStats *stats_out);

/* IRQ functions only clear flags and publish completion/error bits. */
void TraceLink_DmaRxIrqHandler(void);
void TraceLink_DmaTxIrqHandler(void);

#endif /* TRACE_LINK_H */
