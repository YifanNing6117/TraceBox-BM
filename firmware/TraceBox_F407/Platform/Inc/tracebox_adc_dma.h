#ifndef TRACEBOX_ADC_DMA_H
#define TRACEBOX_ADC_DMA_H

#include <stdint.h>

#include "tracebox_types.h"

#define TB_ADC_DMA_SAMPLES_PER_BUFFER  96U
#define TB_ADC_DMA_BUFFER_COUNT         2U
#define TB_ADC_DMA_DESCRIPTOR_CAPACITY  4U

typedef enum
{
    TB_ADC_DMA_STATE_RESET = 0,
    TB_ADC_DMA_STATE_READY,
    TB_ADC_DMA_STATE_RUNNING,
    TB_ADC_DMA_STATE_FAULT
} TbAdcDmaState;

typedef struct
{
    uint32_t sequence;
    uint32_t completed_at_cycles;
    uint16_t sample_count;
    uint8_t buffer_index;
    uint8_t reserved;
} TbAdcDmaDescriptor;

typedef struct
{
    TbAdcDmaState state;
    uint32_t timer_clock_hz;
    uint32_t adc_clock_hz;
    uint32_t sample_rate_hz;
    uint32_t blocks_produced;
    uint32_t blocks_consumed;
    uint32_t dma_irq_count;
    uint32_t dma_error_count;
    uint32_t adc_overrun_count;
    uint32_t descriptor_overflow_count;
    uint32_t descriptor_pending;
    uint32_t descriptor_high_water;
    uint32_t last_isr_cycles;
    uint32_t max_isr_cycles;
} TbAdcDmaStats;

TbStatus TbAdcDma_Init(uint32_t timer_clock_hz,
                       uint32_t apb2_clock_hz,
                       uint32_t sample_rate_hz);
TbStatus TbAdcDma_Start(void);
TbStatus TbAdcDma_Stop(void);
void TbAdcDma_MainFunction(void);
TbStatus TbAdcDma_PopDescriptor(TbAdcDmaDescriptor *descriptor_out);
const uint16_t *TbAdcDma_GetBuffer(uint8_t buffer_index);
void TbAdcDma_GetStats(TbAdcDmaStats *stats_out);

/* IRQ entry clears DMA flags and publishes only completed-buffer metadata. */
void TbAdcDma_IrqHandler(void);

#endif /* TRACEBOX_ADC_DMA_H */
