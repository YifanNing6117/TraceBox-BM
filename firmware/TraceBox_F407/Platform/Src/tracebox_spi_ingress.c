#include "tracebox_spi_ingress.h"

#include "stm32f407xx.h"
#include "tracebox_board.h"

#define TB_SPI_INGRESS_READY_PIN       0U
#define TB_SPI_INGRESS_NSS_PIN         12U
#define TB_SPI_INGRESS_SCK_PIN         13U
#define TB_SPI_INGRESS_MISO_PIN        14U
#define TB_SPI_INGRESS_MOSI_PIN        15U
#define TB_SPI_INGRESS_AF              5U
#define TB_SPI_INGRESS_STOP_SPINS      100000UL
#define TB_SPI_INGRESS_DMA_FLAG_MASK   (DMA_LISR_FEIF3 | DMA_LISR_DMEIF3 | \
                                        DMA_LISR_TEIF3 | DMA_LISR_HTIF3 | \
                                        DMA_LISR_TCIF3)
#define TB_SPI_INGRESS_DMA_CLEAR_MASK  (DMA_LIFCR_CFEIF3 | DMA_LIFCR_CDMEIF3 | \
                                        DMA_LIFCR_CTEIF3 | DMA_LIFCR_CHTIF3 | \
                                        DMA_LIFCR_CTCIF3)
#define TB_SPI_INGRESS_DMA_ERROR_MASK  (DMA_LISR_FEIF3 | DMA_LISR_DMEIF3 | \
                                        DMA_LISR_TEIF3)
#define TB_SPI_INGRESS_SPI_ERROR_MASK  (SPI_SR_OVR | SPI_SR_MODF | \
                                        SPI_SR_CRCERR | SPI_SR_FRE)

typedef struct
{
    TbSpiIngressStats stats;
    uint8_t buffers[TB_SPI_INGRESS_BUFFER_COUNT][TB_TRACE_FRAME_SIZE];
    TbSpiIngressDescriptor descriptors[TB_SPI_INGRESS_DESCRIPTOR_CAPACITY];
    volatile uint32_t descriptor_published;
    volatile uint32_t descriptor_consumed;
    volatile uint32_t buffer_ready_mask;
    volatile uint32_t fault_pending;
    uint32_t next_sequence;
} TbSpiIngressContext;

static TbSpiIngressContext s_ingress;

static void SpiIngress_SetReady(bool ready)
{
    if (ready)
    {
        GPIOE->BSRR = 1UL << TB_SPI_INGRESS_READY_PIN;
        if (!s_ingress.stats.ready_asserted)
        {
            ++s_ingress.stats.ready_high_count;
        }
    }
    else
    {
        GPIOE->BSRR = 1UL << (TB_SPI_INGRESS_READY_PIN + 16U);
        if (s_ingress.stats.ready_asserted)
        {
            ++s_ingress.stats.ready_low_count;
        }
    }
    s_ingress.stats.ready_asserted = ready;
}

static bool SpiIngress_DisableDma(void)
{
    uint32_t spins = TB_SPI_INGRESS_STOP_SPINS;

    DMA1_Stream3->CR &= ~DMA_SxCR_EN;
    while ((DMA1_Stream3->CR & DMA_SxCR_EN) != 0U)
    {
        if (--spins == 0U)
        {
            return false;
        }
    }
    return true;
}

static void SpiIngress_ClearOverrun(void)
{
    volatile uint32_t discard;

    discard = SPI2->DR;
    discard = SPI2->SR;
    (void)discard;
    SPI2->SR &= ~SPI_SR_CRCERR;
}

static void SpiIngress_ConfigureSpi(void)
{
    RCC->APB1RSTR |= RCC_APB1RSTR_SPI2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_SPI2RST;

    /* RM0090: slave, Motorola mode 0, 8-bit, MSB first, hardware NSS. */
    SPI2->CR1 = 0U;
    SPI2->CR2 = SPI_CR2_RXDMAEN | SPI_CR2_ERRIE;
    SpiIngress_ClearOverrun();
    SPI2->CR1 |= SPI_CR1_SPE;
}

static bool SpiIngress_ArmDma(void)
{
    if (!SpiIngress_DisableDma())
    {
        return false;
    }

    DMA1->LIFCR = TB_SPI_INGRESS_DMA_CLEAR_MASK;
    NVIC_ClearPendingIRQ(DMA1_Stream3_IRQn);
    DMA1_Stream3->CR = 0U;
    DMA1_Stream3->FCR = 0U;
    DMA1_Stream3->PAR = (uint32_t)&SPI2->DR;
    DMA1_Stream3->M0AR = (uint32_t)s_ingress.buffers[0];
    DMA1_Stream3->M1AR = (uint32_t)s_ingress.buffers[1];
    DMA1_Stream3->NDTR = TB_TRACE_FRAME_SIZE;
    DMA1_Stream3->CR = DMA_SxCR_PL_1 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
                       DMA_SxCR_DBM | DMA_SxCR_TCIE | DMA_SxCR_TEIE |
                       DMA_SxCR_DMEIE;
    DMA1_Stream3->CR |= DMA_SxCR_EN;
    return true;
}

static bool SpiIngress_ResetHardware(void)
{
    SpiIngress_SetReady(false);
    SPI2->CR2 &= ~SPI_CR2_RXDMAEN;
    SPI2->CR1 &= ~SPI_CR1_SPE;
    if (!SpiIngress_DisableDma())
    {
        return false;
    }
    SpiIngress_ConfigureSpi();
    if (!SpiIngress_ArmDma())
    {
        return false;
    }
    SPI2->CR2 |= SPI_CR2_RXDMAEN;
    return true;
}

static void SpiIngress_ResetRunState(void)
{
    s_ingress.stats.frames_produced = 0U;
    s_ingress.stats.frames_consumed = 0U;
    s_ingress.stats.dma_irq_count = 0U;
    s_ingress.stats.dma_error_count = 0U;
    s_ingress.stats.spi_error_count = 0U;
    s_ingress.stats.short_frame_count = 0U;
    s_ingress.stats.descriptor_overflow_count = 0U;
    s_ingress.stats.descriptor_pending = 0U;
    s_ingress.stats.descriptor_high_water = 0U;
    s_ingress.stats.recovery_count = 0U;
    s_ingress.stats.ready_high_count = 0U;
    s_ingress.stats.ready_low_count = 0U;
    s_ingress.stats.last_isr_cycles = 0U;
    s_ingress.stats.max_isr_cycles = 0U;
    s_ingress.descriptor_published = 0U;
    s_ingress.descriptor_consumed = 0U;
    s_ingress.buffer_ready_mask = 0U;
    s_ingress.fault_pending = 0U;
    s_ingress.next_sequence = 0U;
}

TbStatus TbSpiIngress_Init(void)
{
    uint32_t shift;

    if (!TbBoard_DwtAvailable())
    {
        return TB_STATUS_STATE_ERROR;
    }

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOEEN |
                    RCC_AHB1ENR_DMA1EN;
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;

    GPIOE->BSRR = 1UL << (TB_SPI_INGRESS_READY_PIN + 16U);
    GPIOE->MODER = (GPIOE->MODER & ~(3UL << (TB_SPI_INGRESS_READY_PIN * 2U))) |
                   (1UL << (TB_SPI_INGRESS_READY_PIN * 2U));
    GPIOE->OTYPER &= ~(1UL << TB_SPI_INGRESS_READY_PIN);
    GPIOE->OSPEEDR |= 2UL << (TB_SPI_INGRESS_READY_PIN * 2U);
    GPIOE->PUPDR &= ~(3UL << (TB_SPI_INGRESS_READY_PIN * 2U));

    for (shift = TB_SPI_INGRESS_NSS_PIN;
         shift <= TB_SPI_INGRESS_MOSI_PIN; ++shift)
    {
        uint32_t afr_shift = (shift - 8U) * 4U;

        GPIOB->MODER = (GPIOB->MODER & ~(3UL << (shift * 2U))) |
                       (2UL << (shift * 2U));
        GPIOB->OSPEEDR |= 3UL << (shift * 2U);
        GPIOB->OTYPER &= ~(1UL << shift);
        GPIOB->PUPDR &= ~(3UL << (shift * 2U));
        GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFUL << afr_shift)) |
                        (TB_SPI_INGRESS_AF << afr_shift);
    }
    GPIOB->PUPDR |= 1UL << (TB_SPI_INGRESS_NSS_PIN * 2U); /* NSS idle high. */
    GPIOB->PUPDR |= 2UL << (TB_SPI_INGRESS_SCK_PIN * 2U); /* Mode-0 idle low. */

    SYSCFG->EXTICR[3] = (SYSCFG->EXTICR[3] & ~SYSCFG_EXTICR4_EXTI12) |
                        SYSCFG_EXTICR4_EXTI12_PB;
    EXTI->IMR |= EXTI_IMR_MR12;
    EXTI->RTSR |= EXTI_RTSR_TR12;
    EXTI->FTSR &= ~EXTI_FTSR_TR12;
    EXTI->PR = EXTI_PR_PR12;

    SpiIngress_ResetRunState();
    SpiIngress_ConfigureSpi();
    if (!SpiIngress_ArmDma())
    {
        s_ingress.stats.state = TB_SPI_INGRESS_STATE_FAULT;
        return TB_STATUS_HW_ERROR;
    }

    NVIC_SetPriority(DMA1_Stream3_IRQn, 2U);
    NVIC_EnableIRQ(DMA1_Stream3_IRQn);
    NVIC_SetPriority(SPI2_IRQn, 3U);
    NVIC_EnableIRQ(SPI2_IRQn);
    NVIC_SetPriority(EXTI15_10_IRQn, 3U);
    NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
    NVIC_EnableIRQ(EXTI15_10_IRQn);

    s_ingress.stats.state = TB_SPI_INGRESS_STATE_STOPPED;
    SpiIngress_SetReady(false);
    return TB_STATUS_OK;
}

TbStatus TbSpiIngress_Start(void)
{
    if (s_ingress.stats.state != TB_SPI_INGRESS_STATE_STOPPED)
    {
        return TB_STATUS_BUSY;
    }

    SpiIngress_ResetRunState();
    if (!SpiIngress_ResetHardware())
    {
        s_ingress.stats.state = TB_SPI_INGRESS_STATE_FAULT;
        return TB_STATUS_HW_ERROR;
    }
    s_ingress.stats.state = TB_SPI_INGRESS_STATE_RUNNING;
    SpiIngress_SetReady(true);
    return TB_STATUS_OK;
}

TbStatus TbSpiIngress_Stop(void)
{
    if ((s_ingress.stats.state != TB_SPI_INGRESS_STATE_RUNNING) &&
        (s_ingress.stats.state != TB_SPI_INGRESS_STATE_RECOVERING) &&
        (s_ingress.stats.state != TB_SPI_INGRESS_STATE_FAULT))
    {
        return TB_STATUS_STATE_ERROR;
    }

    SpiIngress_SetReady(false);
    SPI2->CR2 &= ~SPI_CR2_RXDMAEN;
    if (!SpiIngress_DisableDma())
    {
        s_ingress.stats.state = TB_SPI_INGRESS_STATE_FAULT;
        return TB_STATUS_HW_ERROR;
    }
    /* A master may have launched one boundary frame before observing READY
     * low.  Once the capture target stops ingress, discard that unpublished
     * credit explicitly so the next run starts with an empty descriptor set. */
    s_ingress.descriptor_consumed = s_ingress.descriptor_published;
    s_ingress.buffer_ready_mask = 0U;
    s_ingress.stats.descriptor_pending = 0U;
    __DMB();
    s_ingress.stats.state = TB_SPI_INGRESS_STATE_STOPPED;
    return TB_STATUS_OK;
}

void TbSpiIngress_MainFunction(void)
{
    if ((s_ingress.stats.state == TB_SPI_INGRESS_STATE_RUNNING) &&
        (s_ingress.fault_pending != 0U))
    {
        s_ingress.stats.state = TB_SPI_INGRESS_STATE_RECOVERING;
        s_ingress.descriptor_published = 0U;
        s_ingress.descriptor_consumed = 0U;
        s_ingress.buffer_ready_mask = 0U;
        s_ingress.fault_pending = 0U;
        if (SpiIngress_ResetHardware())
        {
            ++s_ingress.stats.recovery_count;
            s_ingress.stats.state = TB_SPI_INGRESS_STATE_RUNNING;
            SpiIngress_SetReady(true);
        }
        else
        {
            s_ingress.stats.state = TB_SPI_INGRESS_STATE_FAULT;
        }
    }
}

TbStatus TbSpiIngress_PopDescriptor(TbSpiIngressDescriptor *descriptor_out)
{
    uint32_t consumed;

    if (descriptor_out == 0)
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }
    consumed = s_ingress.descriptor_consumed;
    if (consumed == s_ingress.descriptor_published)
    {
        return TB_STATUS_BUSY;
    }

    __DMB();
    *descriptor_out = s_ingress.descriptors[
        consumed & (TB_SPI_INGRESS_DESCRIPTOR_CAPACITY - 1U)];
    __DMB();
    s_ingress.descriptor_consumed = consumed + 1U;
    ++s_ingress.stats.frames_consumed;
    s_ingress.stats.descriptor_pending =
        s_ingress.descriptor_published - s_ingress.descriptor_consumed;
    return TB_STATUS_OK;
}

const uint8_t *TbSpiIngress_GetBuffer(uint8_t buffer_index)
{
    if ((buffer_index >= TB_SPI_INGRESS_BUFFER_COUNT) ||
        ((s_ingress.buffer_ready_mask & (1UL << buffer_index)) == 0U))
    {
        return 0;
    }
    return s_ingress.buffers[buffer_index];
}

TbStatus TbSpiIngress_ReleaseBuffer(uint8_t buffer_index)
{
    uint32_t mask;

    if (buffer_index >= TB_SPI_INGRESS_BUFFER_COUNT)
    {
        return TB_STATUS_RANGE_ERROR;
    }
    mask = 1UL << buffer_index;
    if ((s_ingress.buffer_ready_mask & mask) == 0U)
    {
        return TB_STATUS_STATE_ERROR;
    }

    s_ingress.buffer_ready_mask &= ~mask;
    __DMB();
    return TB_STATUS_OK;
}

TbStatus TbSpiIngress_GrantCredit(void)
{
    if (s_ingress.stats.state != TB_SPI_INGRESS_STATE_RUNNING)
    {
        return TB_STATUS_STATE_ERROR;
    }
    if ((s_ingress.buffer_ready_mask != 0U) ||
        (s_ingress.fault_pending != 0U))
    {
        return TB_STATUS_BUSY;
    }

    SpiIngress_SetReady(true);
    return TB_STATUS_OK;
}

void TbSpiIngress_GetStats(TbSpiIngressStats *stats_out)
{
    if (stats_out != 0)
    {
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        *stats_out = s_ingress.stats;
        stats_out->descriptor_pending =
            s_ingress.descriptor_published - s_ingress.descriptor_consumed;
        if (primask == 0U)
        {
            __enable_irq();
        }
    }
}

void TbSpiIngress_DmaRxIrqHandler(void)
{
    uint32_t started_at = TbBoard_DwtCycles();
    uint32_t flags = DMA1->LISR & TB_SPI_INGRESS_DMA_FLAG_MASK;

    if (flags != 0U)
    {
        DMA1->LIFCR = TB_SPI_INGRESS_DMA_CLEAR_MASK;
        ++s_ingress.stats.dma_irq_count;
    }
    if ((flags & TB_SPI_INGRESS_DMA_ERROR_MASK) != 0U)
    {
        ++s_ingress.stats.dma_error_count;
        s_ingress.fault_pending = 1U;
        SpiIngress_SetReady(false);
    }
    else if ((flags & DMA_LISR_TCIF3) != 0U)
    {
        uint8_t completed_buffer =
            ((DMA1_Stream3->CR & DMA_SxCR_CT) != 0U) ? 0U : 1U;
        uint32_t buffer_mask = 1UL << completed_buffer;
        uint32_t published = s_ingress.descriptor_published;
        uint32_t depth = published - s_ingress.descriptor_consumed;

        ++s_ingress.stats.frames_produced;
        SpiIngress_SetReady(false);
        if (((s_ingress.buffer_ready_mask & buffer_mask) != 0U) ||
            (depth >= TB_SPI_INGRESS_DESCRIPTOR_CAPACITY))
        {
            ++s_ingress.stats.descriptor_overflow_count;
            s_ingress.fault_pending = 1U;
        }
        else
        {
            TbSpiIngressDescriptor *descriptor = &s_ingress.descriptors[
                published & (TB_SPI_INGRESS_DESCRIPTOR_CAPACITY - 1U)];

            s_ingress.buffer_ready_mask |= buffer_mask;
            descriptor->sequence = s_ingress.next_sequence++;
            descriptor->completed_at_cycles = started_at;
            descriptor->byte_count = TB_TRACE_FRAME_SIZE;
            descriptor->buffer_index = completed_buffer;
            descriptor->hardware_status = (uint8_t)(SPI2->SR & 0xFFU);
            __DMB();
            s_ingress.descriptor_published = published + 1U;
            ++depth;
            if (depth > s_ingress.stats.descriptor_high_water)
            {
                s_ingress.stats.descriptor_high_water = depth;
            }
        }
    }

    s_ingress.stats.last_isr_cycles = TbBoard_DwtCycles() - started_at;
    if (s_ingress.stats.last_isr_cycles > s_ingress.stats.max_isr_cycles)
    {
        s_ingress.stats.max_isr_cycles = s_ingress.stats.last_isr_cycles;
    }
}

void TbSpiIngress_SpiIrqHandler(void)
{
    uint32_t status = SPI2->SR;

    if ((status & TB_SPI_INGRESS_SPI_ERROR_MASK) != 0U)
    {
        volatile uint32_t discard;

        /* Acknowledge the source before publishing it; otherwise OVR can
         * continuously re-enter the IRQ and starve foreground recovery. */
        SPI2->CR2 &= ~SPI_CR2_ERRIE;
        if ((status & SPI_SR_OVR) != 0U)
        {
            discard = SPI2->DR;
            discard = SPI2->SR;
            (void)discard;
        }
        if ((status & SPI_SR_CRCERR) != 0U)
        {
            SPI2->SR &= ~SPI_SR_CRCERR;
        }
        ++s_ingress.stats.spi_error_count;
        s_ingress.fault_pending = 1U;
        SpiIngress_SetReady(false);
    }
}

void TbSpiIngress_NssRisingIrqHandler(void)
{
    if ((s_ingress.stats.state == TB_SPI_INGRESS_STATE_RUNNING) &&
        (DMA1_Stream3->NDTR != TB_TRACE_FRAME_SIZE))
    {
        ++s_ingress.stats.short_frame_count;
        s_ingress.fault_pending = 1U;
        SpiIngress_SetReady(false);
    }
}
