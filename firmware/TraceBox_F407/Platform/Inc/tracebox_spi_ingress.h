#ifndef TRACEBOX_SPI_INGRESS_H
#define TRACEBOX_SPI_INGRESS_H

#include <stdbool.h>
#include <stdint.h>

#include "tracebox_trace_frame.h"
#include "tracebox_types.h"

#define TB_SPI_INGRESS_BUFFER_COUNT        2U
#define TB_SPI_INGRESS_DESCRIPTOR_CAPACITY 4U

typedef enum
{
    TB_SPI_INGRESS_STATE_RESET = 0,
    TB_SPI_INGRESS_STATE_STOPPED,
    TB_SPI_INGRESS_STATE_RUNNING,
    TB_SPI_INGRESS_STATE_RECOVERING,
    TB_SPI_INGRESS_STATE_FAULT
} TbSpiIngressState;

typedef struct
{
    uint32_t sequence;
    uint32_t completed_at_cycles;
    uint16_t byte_count;
    uint8_t buffer_index;
    uint8_t hardware_status;
} TbSpiIngressDescriptor;

typedef struct
{
    TbSpiIngressState state;
    uint32_t frames_produced;
    uint32_t frames_consumed;
    uint32_t dma_irq_count;
    uint32_t dma_error_count;
    uint32_t spi_error_count;
    uint32_t short_frame_count;
    uint32_t descriptor_overflow_count;
    uint32_t descriptor_pending;
    uint32_t descriptor_high_water;
    uint32_t recovery_count;
    uint32_t ready_high_count;
    uint32_t ready_low_count;
    uint32_t last_isr_cycles;
    uint32_t max_isr_cycles;
    bool ready_asserted;
} TbSpiIngressStats;

TbStatus TbSpiIngress_Init(void);
TbStatus TbSpiIngress_Start(void);
TbStatus TbSpiIngress_Stop(void);
void TbSpiIngress_MainFunction(void);
TbStatus TbSpiIngress_PopDescriptor(TbSpiIngressDescriptor *descriptor_out);
const uint8_t *TbSpiIngress_GetBuffer(uint8_t buffer_index);
TbStatus TbSpiIngress_ReleaseBuffer(uint8_t buffer_index);
/* Grant exactly one new master transaction after downstream storage frees
 * capacity. READY stays low between grants, providing end-to-end credit. */
TbStatus TbSpiIngress_GrantCredit(void);
void TbSpiIngress_GetStats(TbSpiIngressStats *stats_out);

/* IRQ entries publish metadata/errors only; parsing and recovery run in main. */
void TbSpiIngress_DmaRxIrqHandler(void);
void TbSpiIngress_SpiIrqHandler(void);
void TbSpiIngress_NssRisingIrqHandler(void);

#endif /* TRACEBOX_SPI_INGRESS_H */
