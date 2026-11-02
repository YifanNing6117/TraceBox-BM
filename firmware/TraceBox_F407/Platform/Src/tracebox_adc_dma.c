#include "tracebox_adc_dma.h"

#include <stdbool.h>

#include "stm32f407xx.h"
#include "tracebox_board.h"

#define TB_ADC_PIN                    0U
#define TB_ADC_DMA_FLAG_MASK          (DMA_HISR_FEIF4 | DMA_HISR_DMEIF4 | \
                                       DMA_HISR_TEIF4 | DMA_HISR_HTIF4 | \
                                       DMA_HISR_TCIF4)
#define TB_ADC_DMA_ERROR_MASK         (DMA_HISR_DMEIF4 | DMA_HISR_TEIF4)
#define TB_ADC_TIMER_BASE_HZ          1000000UL
#define TB_ADC_EXTSEL_TIM2_TRGO       (6UL << ADC_CR2_EXTSEL_Pos)
#define TB_ADC_SAMPLE_TIME_56_CYCLES  (3UL << ADC_SMPR2_SMP0_Pos)
#define TB_ADC_DMA_STOP_SPINS         100000UL

typedef struct
{
    TbAdcDmaStats stats;
    uint16_t buffers[TB_ADC_DMA_BUFFER_COUNT][TB_ADC_DMA_SAMPLES_PER_BUFFER];
    TbAdcDmaDescriptor descriptors[TB_ADC_DMA_DESCRIPTOR_CAPACITY];
    volatile uint32_t descriptor_published;
    volatile uint32_t descriptor_consumed;
    volatile uint32_t fault_pending;
    uint32_t next_sequence;
    uint32_t timer_prescaler;
    uint32_t timer_period;
} TbAdcDmaContext;

static TbAdcDmaContext s_adc;

static bool AdcDma_DisableStream(void)
{
    uint32_t spins = TB_ADC_DMA_STOP_SPINS;

    DMA2_Stream4->CR &= ~DMA_SxCR_EN;
    while ((DMA2_Stream4->CR & DMA_SxCR_EN) != 0U)
    {
        if (--spins == 0U)
        {
            return false;
        }
    }
    return true;
}

static bool AdcDma_StopHardware(void)
{
    TIM2->CR1 &= ~TIM_CR1_CEN;
    ADC1->CR2 &= ~ADC_CR2_EXTEN;
    ADC1->CR2 &= ~(ADC_CR2_DMA | ADC_CR2_DDS);

    if (!AdcDma_DisableStream())
    {
        return false;
    }

    ADC1->CR2 &= ~ADC_CR2_ADON;
    DMA2->HIFCR = TB_ADC_DMA_FLAG_MASK;
    NVIC_ClearPendingIRQ(DMA2_Stream4_IRQn);
    return true;
}

static void AdcDma_ResetRunMetrics(void)
{
    s_adc.stats.blocks_produced = 0U;
    s_adc.stats.blocks_consumed = 0U;
    s_adc.stats.dma_irq_count = 0U;
    s_adc.stats.dma_error_count = 0U;
    s_adc.stats.adc_overrun_count = 0U;
    s_adc.stats.descriptor_overflow_count = 0U;
    s_adc.stats.descriptor_pending = 0U;
    s_adc.stats.descriptor_high_water = 0U;
    s_adc.stats.last_isr_cycles = 0U;
    s_adc.stats.max_isr_cycles = 0U;
    s_adc.descriptor_published = 0U;
    s_adc.descriptor_consumed = 0U;
    s_adc.fault_pending = 0U;
    s_adc.next_sequence = 0U;
}

TbStatus TbAdcDma_Init(uint32_t timer_clock_hz,
                       uint32_t apb2_clock_hz,
                       uint32_t sample_rate_hz)
{
    uint32_t timer_divider;
    uint32_t adc_divider = 4U;

    if ((timer_clock_hz == 0U) || (apb2_clock_hz == 0U) ||
        (sample_rate_hz == 0U) ||
        ((timer_clock_hz % TB_ADC_TIMER_BASE_HZ) != 0U) ||
        ((TB_ADC_TIMER_BASE_HZ % sample_rate_hz) != 0U) ||
        !TbBoard_DwtAvailable())
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    timer_divider = timer_clock_hz / TB_ADC_TIMER_BASE_HZ;
    s_adc.timer_prescaler = timer_divider - 1U;
    s_adc.timer_period = (TB_ADC_TIMER_BASE_HZ / sample_rate_hz) - 1U;
    if ((s_adc.timer_prescaler > 0xFFFFU) ||
        ((apb2_clock_hz / adc_divider) > 36000000UL))
    {
        return TB_STATUS_RANGE_ERROR;
    }

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_DMA2EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    (void)RCC->APB2ENR;

    GPIOA->MODER |= 3UL << (TB_ADC_PIN * 2U);
    GPIOA->PUPDR &= ~(3UL << (TB_ADC_PIN * 2U));

    RCC->APB1RSTR |= RCC_APB1RSTR_TIM2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM2RST;
    RCC->APB2RSTR |= RCC_APB2RSTR_ADCRST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_ADCRST;

    TIM2->CR1 = 0U;
    TIM2->CR2 = TIM_CR2_MMS_1; /* Update event is the TRGO source. */
    TIM2->PSC = s_adc.timer_prescaler;
    TIM2->ARR = s_adc.timer_period;
    TIM2->CNT = 0U;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0U;

    ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0; /* /4 */
    ADC1->CR1 = 0U;
    ADC1->CR2 = 0U;
    ADC1->SMPR1 = 0U;
    ADC1->SMPR2 = (ADC1->SMPR2 & ~ADC_SMPR2_SMP0) |
                  TB_ADC_SAMPLE_TIME_56_CYCLES;
    ADC1->SQR1 = 0U;
    ADC1->SQR2 = 0U;
    ADC1->SQR3 = 0U; /* One regular conversion: channel 0. */
    ADC1->SR = 0U;

    if (!AdcDma_DisableStream())
    {
        return TB_STATUS_HW_ERROR;
    }
    DMA2_Stream4->CR = 0U;
    DMA2_Stream4->FCR = 0U;
    DMA2->HIFCR = TB_ADC_DMA_FLAG_MASK;

    NVIC_SetPriority(DMA2_Stream4_IRQn, 2U);
    NVIC_ClearPendingIRQ(DMA2_Stream4_IRQn);
    NVIC_EnableIRQ(DMA2_Stream4_IRQn);

    s_adc.stats.state = TB_ADC_DMA_STATE_READY;
    s_adc.stats.timer_clock_hz = timer_clock_hz;
    s_adc.stats.adc_clock_hz = apb2_clock_hz / adc_divider;
    s_adc.stats.sample_rate_hz = sample_rate_hz;
    AdcDma_ResetRunMetrics();
    return TB_STATUS_OK;
}

TbStatus TbAdcDma_Start(void)
{
    if (s_adc.stats.state != TB_ADC_DMA_STATE_READY)
    {
        return TB_STATUS_BUSY;
    }

    if (!AdcDma_DisableStream())
    {
        s_adc.stats.state = TB_ADC_DMA_STATE_FAULT;
        return TB_STATUS_HW_ERROR;
    }

    AdcDma_ResetRunMetrics();
    DMA2->HIFCR = TB_ADC_DMA_FLAG_MASK;
    NVIC_ClearPendingIRQ(DMA2_Stream4_IRQn);
    ADC1->SR = 0U;

    DMA2_Stream4->PAR = (uint32_t)&ADC1->DR;
    DMA2_Stream4->M0AR = (uint32_t)s_adc.buffers[0];
    DMA2_Stream4->M1AR = (uint32_t)s_adc.buffers[1];
    DMA2_Stream4->NDTR = TB_ADC_DMA_SAMPLES_PER_BUFFER;
    DMA2_Stream4->FCR = 0U;
    DMA2_Stream4->CR = DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_0 |
                       DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
                       DMA_SxCR_DBM | DMA_SxCR_TCIE | DMA_SxCR_TEIE |
                       DMA_SxCR_DMEIE;

    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->CNT = 0U;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0U;

    DMA2_Stream4->CR |= DMA_SxCR_EN;
    ADC1->CR2 = ADC_CR2_DMA | ADC_CR2_DDS | TB_ADC_EXTSEL_TIM2_TRGO |
                ADC_CR2_EXTEN_0 | ADC_CR2_ADON;
    __DSB();
    s_adc.stats.state = TB_ADC_DMA_STATE_RUNNING;
    TIM2->CR1 |= TIM_CR1_CEN;
    return TB_STATUS_OK;
}

TbStatus TbAdcDma_Stop(void)
{
    if ((s_adc.stats.state != TB_ADC_DMA_STATE_RUNNING) &&
        (s_adc.stats.state != TB_ADC_DMA_STATE_FAULT))
    {
        return TB_STATUS_STATE_ERROR;
    }

    if (!AdcDma_StopHardware())
    {
        s_adc.stats.state = TB_ADC_DMA_STATE_FAULT;
        return TB_STATUS_HW_ERROR;
    }

    s_adc.stats.state = TB_ADC_DMA_STATE_READY;
    s_adc.stats.descriptor_pending =
        s_adc.descriptor_published - s_adc.descriptor_consumed;
    return TB_STATUS_OK;
}

void TbAdcDma_MainFunction(void)
{
    if ((s_adc.stats.state == TB_ADC_DMA_STATE_RUNNING) &&
        (s_adc.fault_pending != 0U))
    {
        (void)AdcDma_StopHardware();
        s_adc.stats.state = TB_ADC_DMA_STATE_FAULT;
    }
}

TbStatus TbAdcDma_PopDescriptor(TbAdcDmaDescriptor *descriptor_out)
{
    uint32_t consumed;

    if (descriptor_out == 0)
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    consumed = s_adc.descriptor_consumed;
    if (consumed == s_adc.descriptor_published)
    {
        return TB_STATUS_BUSY;
    }

    __DMB();
    *descriptor_out = s_adc.descriptors[
        consumed & (TB_ADC_DMA_DESCRIPTOR_CAPACITY - 1U)];
    __DMB();
    s_adc.descriptor_consumed = consumed + 1U;
    ++s_adc.stats.blocks_consumed;
    s_adc.stats.descriptor_pending =
        s_adc.descriptor_published - s_adc.descriptor_consumed;
    return TB_STATUS_OK;
}

const uint16_t *TbAdcDma_GetBuffer(uint8_t buffer_index)
{
    if (buffer_index >= TB_ADC_DMA_BUFFER_COUNT)
    {
        return 0;
    }
    return s_adc.buffers[buffer_index];
}

void TbAdcDma_GetStats(TbAdcDmaStats *stats_out)
{
    if (stats_out != 0)
    {
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        *stats_out = s_adc.stats;
        stats_out->descriptor_pending =
            s_adc.descriptor_published - s_adc.descriptor_consumed;
        if (primask == 0U)
        {
            __enable_irq();
        }
    }
}

void TbAdcDma_IrqHandler(void)
{
    uint32_t started_at = TbBoard_DwtCycles();
    uint32_t flags = DMA2->HISR & TB_ADC_DMA_FLAG_MASK;
    bool error = false;

    if (flags != 0U)
    {
        DMA2->HIFCR = flags;
        ++s_adc.stats.dma_irq_count;
    }

    if ((flags & TB_ADC_DMA_ERROR_MASK) != 0U)
    {
        ++s_adc.stats.dma_error_count;
        error = true;
    }
    if ((ADC1->SR & ADC_SR_OVR) != 0U)
    {
        ADC1->SR &= ~ADC_SR_OVR;
        ++s_adc.stats.adc_overrun_count;
        error = true;
    }

    if (error)
    {
        s_adc.fault_pending = 1U;
    }
    else if ((flags & DMA_HISR_TCIF4) != 0U)
    {
        uint32_t published = s_adc.descriptor_published;
        uint32_t depth = published - s_adc.descriptor_consumed;

        ++s_adc.stats.blocks_produced;
        if (depth < TB_ADC_DMA_DESCRIPTOR_CAPACITY)
        {
            TbAdcDmaDescriptor *descriptor = &s_adc.descriptors[
                published & (TB_ADC_DMA_DESCRIPTOR_CAPACITY - 1U)];

            descriptor->sequence = s_adc.next_sequence;
            descriptor->completed_at_cycles = started_at;
            descriptor->sample_count = TB_ADC_DMA_SAMPLES_PER_BUFFER;
            descriptor->buffer_index =
                ((DMA2_Stream4->CR & DMA_SxCR_CT) != 0U) ? 0U : 1U;
            descriptor->reserved = 0U;
            ++s_adc.next_sequence;
            __DMB();
            s_adc.descriptor_published = published + 1U;
            ++depth;
            if (depth > s_adc.stats.descriptor_high_water)
            {
                s_adc.stats.descriptor_high_water = depth;
            }
        }
        else
        {
            ++s_adc.next_sequence;
            ++s_adc.stats.descriptor_overflow_count;
        }
    }

    s_adc.stats.last_isr_cycles = TbBoard_DwtCycles() - started_at;
    if (s_adc.stats.last_isr_cycles > s_adc.stats.max_isr_cycles)
    {
        s_adc.stats.max_isr_cycles = s_adc.stats.last_isr_cycles;
    }
}
