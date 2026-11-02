#include "trace_link.h"

#include <string.h>

#include "main.h"

#define TRACE_LINK_QUEUE_CAPACITY 32u
#define TRACE_LINK_QUEUE_MASK     (TRACE_LINK_QUEUE_CAPACITY - 1u)
#define TRACE_LINK_READY_PIN      1u
#define TRACE_LINK_CS_PIN         12u
#define TRACE_LINK_SCK_PIN        13u
#define TRACE_LINK_MISO_PIN       14u
#define TRACE_LINK_MOSI_PIN       15u
#define TRACE_LINK_AF             5u
#define TRACE_LINK_TIMEOUT_MS     5u
#define TRACE_LINK_STOP_SPINS     100000u

#define TRACE_LINK_EVENT_RX_DONE  (1u << 0)
#define TRACE_LINK_EVENT_TX_DONE  (1u << 1)
#define TRACE_LINK_EVENT_ERROR    (1u << 2)

#define TRACE_LINK_RX_FLAG_MASK   (DMA_LISR_FEIF3 | DMA_LISR_DMEIF3 | \
                                   DMA_LISR_TEIF3 | DMA_LISR_HTIF3 | \
                                   DMA_LISR_TCIF3)
#define TRACE_LINK_RX_CLEAR_MASK  (DMA_LIFCR_CFEIF3 | DMA_LIFCR_CDMEIF3 | \
                                   DMA_LIFCR_CTEIF3 | DMA_LIFCR_CHTIF3 | \
                                   DMA_LIFCR_CTCIF3)
#define TRACE_LINK_RX_ERROR_MASK  (DMA_LISR_DMEIF3 | DMA_LISR_TEIF3)
#define TRACE_LINK_TX_FLAG_MASK   (DMA_HISR_FEIF4 | DMA_HISR_DMEIF4 | \
                                   DMA_HISR_TEIF4 | DMA_HISR_HTIF4 | \
                                   DMA_HISR_TCIF4)
#define TRACE_LINK_TX_CLEAR_MASK  (DMA_HIFCR_CFEIF4 | DMA_HIFCR_CDMEIF4 | \
                                   DMA_HIFCR_CTEIF4 | DMA_HIFCR_CHTIF4 | \
                                   DMA_HIFCR_CTCIF4)
#define TRACE_LINK_TX_ERROR_MASK  (DMA_HISR_DMEIF4 | DMA_HISR_TEIF4)

typedef struct {
    uint8_t bytes[TRACE_LINK_FRAME_SIZE];
} TraceLinkFrame;

typedef struct {
    TraceLinkStats stats;
    TraceLinkFrame queue[TRACE_LINK_QUEUE_CAPACITY];
    uint8_t rx_sink[TRACE_LINK_FRAME_SIZE];
    uint32_t sequence_by_source[3];
    uint32_t queue_head;
    uint32_t queue_tail;
    uint32_t deadline_ms;
    uint32_t next_start_ms;
    volatile uint32_t irq_events;
    uint8_t inject_bad_crc_once;
} TraceLinkContext;

static TraceLinkContext s_trace;

static void TraceLink_WriteU16Le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void TraceLink_WriteU32Le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static uint32_t TraceLink_Crc32Mpeg2(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t index;

    for (index = 0u; index < length; ++index) {
        uint8_t bit;

        crc ^= (uint32_t)data[index] << 24u;
        for (bit = 0u; bit < 8u; ++bit) {
            crc = ((crc & 0x80000000u) != 0u) ?
                  ((crc << 1u) ^ 0x04C11DB7u) : (crc << 1u);
        }
    }
    return crc;
}

static uint8_t TraceLink_Ready(void)
{
    return ((GPIOE->IDR & (1u << TRACE_LINK_READY_PIN)) != 0u) ? 1u : 0u;
}

static void TraceLink_CsHigh(void)
{
    GPIOB->BSRR = 1u << TRACE_LINK_CS_PIN;
}

static void TraceLink_CsLow(void)
{
    GPIOB->BSRR = 1u << (TRACE_LINK_CS_PIN + 16u);
}

static uint8_t TraceLink_DisableStream(DMA_Stream_TypeDef *stream)
{
    uint32_t spins = TRACE_LINK_STOP_SPINS;

    stream->CR &= ~DMA_SxCR_EN;
    while ((stream->CR & DMA_SxCR_EN) != 0u) {
        if (--spins == 0u) {
            return 0u;
        }
    }
    return 1u;
}

static void TraceLink_ClearSpi(void)
{
    volatile uint32_t discard;

    discard = SPI2->DR;
    discard = SPI2->SR;
    (void)discard;
    SPI2->SR &= ~SPI_SR_CRCERR;
}

static uint8_t TraceLink_ConfigureSpi(void)
{
    if ((TraceLink_DisableStream(DMA1_Stream3) == 0u) ||
        (TraceLink_DisableStream(DMA1_Stream4) == 0u)) {
        return 0u;
    }

    SPI2->CR2 = 0u;
    SPI2->CR1 &= ~SPI_CR1_SPE;
    RCC->APB1RSTR |= RCC_APB1RSTR_SPI2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_SPI2RST;
    /* APB1=42 MHz, BR=100b gives 1.3125 MHz. Mode 0, 8 bit, MSB first. */
    SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_BR_2 | SPI_CR1_SSM | SPI_CR1_SSI;
    SPI2->CR2 = 0u;
    TraceLink_ClearSpi();
    SPI2->CR1 |= SPI_CR1_SPE;
    DMA1->LIFCR = TRACE_LINK_RX_CLEAR_MASK;
    DMA1->HIFCR = TRACE_LINK_TX_CLEAR_MASK;
    NVIC_ClearPendingIRQ(DMA1_Stream3_IRQn);
    NVIC_ClearPendingIRQ(DMA1_Stream4_IRQn);
    return 1u;
}

static void TraceLink_Recover(TraceLinkStatus reason, uint32_t now_ms)
{
    s_trace.stats.state = TRACE_LINK_STATE_RECOVERING;
    TraceLink_CsHigh();
    SPI2->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    if (reason == TRACE_LINK_ERR_TIMEOUT) {
        ++s_trace.stats.timeout_count;
    } else {
        ++s_trace.stats.dma_error_count;
    }
    if (TraceLink_ConfigureSpi() != 0u) {
        ++s_trace.stats.recovery_count;
        s_trace.irq_events = 0u;
        s_trace.next_start_ms = now_ms + 1u;
        s_trace.stats.state = TRACE_LINK_STATE_IDLE;
    } else {
        s_trace.stats.state = TRACE_LINK_STATE_FAULT;
    }
}

static TraceLinkStatus TraceLink_StartDma(uint32_t now_ms)
{
    const uint8_t *tx;

    if ((s_trace.stats.state != TRACE_LINK_STATE_IDLE) ||
        (s_trace.queue_tail == s_trace.queue_head)) {
        return TRACE_LINK_ERR_BUSY;
    }
    if (TraceLink_Ready() == 0u) {
        ++s_trace.stats.backpressure_count;
        return TRACE_LINK_ERR_BUSY;
    }
    if ((TraceLink_DisableStream(DMA1_Stream3) == 0u) ||
        (TraceLink_DisableStream(DMA1_Stream4) == 0u)) {
        return TRACE_LINK_ERR_HW;
    }

    tx = s_trace.queue[s_trace.queue_tail & TRACE_LINK_QUEUE_MASK].bytes;
    DMA1->LIFCR = TRACE_LINK_RX_CLEAR_MASK;
    DMA1->HIFCR = TRACE_LINK_TX_CLEAR_MASK;
    s_trace.irq_events = 0u;

    DMA1_Stream3->CR = 0u;
    DMA1_Stream3->FCR = 0u;
    DMA1_Stream3->PAR = (uint32_t)&SPI2->DR;
    DMA1_Stream3->M0AR = (uint32_t)s_trace.rx_sink;
    DMA1_Stream3->NDTR = TRACE_LINK_FRAME_SIZE;
    DMA1_Stream3->CR = DMA_SxCR_PL_1 | DMA_SxCR_MINC |
                       DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;

    DMA1_Stream4->CR = 0u;
    DMA1_Stream4->FCR = 0u;
    DMA1_Stream4->PAR = (uint32_t)&SPI2->DR;
    DMA1_Stream4->M0AR = (uint32_t)tx;
    DMA1_Stream4->NDTR = TRACE_LINK_FRAME_SIZE;
    DMA1_Stream4->CR = DMA_SxCR_PL_1 | DMA_SxCR_MINC | DMA_SxCR_DIR_0 |
                       DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;

    TraceLink_CsLow();
    s_trace.deadline_ms = now_ms + TRACE_LINK_TIMEOUT_MS;
    s_trace.stats.state = TRACE_LINK_STATE_DMA_ACTIVE;
    __DMB();
    SPI2->CR2 |= SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    DMA1_Stream3->CR |= DMA_SxCR_EN;
    DMA1_Stream4->CR |= DMA_SxCR_EN;
    return TRACE_LINK_OK;
}

TraceLinkStatus TraceLink_Init(void)
{
    uint32_t pin;
    uint8_t boot_payload[4];

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOEEN |
                    RCC_AHB1ENR_DMA1EN;
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
    (void)RCC->APB1ENR;

    TraceLink_CsHigh();
    GPIOB->MODER = (GPIOB->MODER & ~(3u << (TRACE_LINK_CS_PIN * 2u))) |
                   (1u << (TRACE_LINK_CS_PIN * 2u));
    GPIOB->OTYPER &= ~(1u << TRACE_LINK_CS_PIN);
    GPIOB->OSPEEDR |= 3u << (TRACE_LINK_CS_PIN * 2u);
    GPIOB->PUPDR &= ~(3u << (TRACE_LINK_CS_PIN * 2u));

    for (pin = TRACE_LINK_SCK_PIN; pin <= TRACE_LINK_MOSI_PIN; ++pin) {
        uint32_t afr_shift = (pin - 8u) * 4u;

        GPIOB->MODER = (GPIOB->MODER & ~(3u << (pin * 2u))) |
                       (2u << (pin * 2u));
        GPIOB->OTYPER &= ~(1u << pin);
        GPIOB->OSPEEDR |= 3u << (pin * 2u);
        GPIOB->PUPDR &= ~(3u << (pin * 2u));
        GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFu << afr_shift)) |
                        (TRACE_LINK_AF << afr_shift);
    }

    GPIOE->MODER &= ~(3u << (TRACE_LINK_READY_PIN * 2u));
    GPIOE->PUPDR = (GPIOE->PUPDR & ~(3u << (TRACE_LINK_READY_PIN * 2u))) |
                   (2u << (TRACE_LINK_READY_PIN * 2u));

    memset(&s_trace, 0, sizeof(s_trace));
    if (TraceLink_ConfigureSpi() == 0u) {
        s_trace.stats.state = TRACE_LINK_STATE_FAULT;
        return TRACE_LINK_ERR_HW;
    }
    NVIC_SetPriority(DMA1_Stream3_IRQn, 6u);
    NVIC_SetPriority(DMA1_Stream4_IRQn, 6u);
    NVIC_EnableIRQ(DMA1_Stream3_IRQn);
    NVIC_EnableIRQ(DMA1_Stream4_IRQn);
    s_trace.stats.state = TRACE_LINK_STATE_IDLE;

    TraceLink_WriteU32Le(boot_payload, RCC->CSR);
    (void)TraceLink_Emit(TRACE_SOURCE_TESTER, TRACE_EVENT_BOOT,
                         TRACE_SEVERITY_INFO, boot_payload,
                         sizeof(boot_payload), HAL_GetTick());
    return TRACE_LINK_OK;
}

TraceLinkStatus TraceLink_Emit(uint8_t source_node,
                               uint16_t event_id,
                               uint8_t severity,
                               const uint8_t *payload,
                               uint8_t payload_length,
                               uint32_t timestamp_ms)
{
    TraceLinkFrame *frame;
    uint32_t crc;
    uint32_t depth;

    if ((source_node < TRACE_SOURCE_TESTER) ||
        (source_node > TRACE_SOURCE_ECU) ||
        (payload_length > TRACE_LINK_PAYLOAD_CAPACITY) ||
        ((payload_length > 0u) && (payload == 0))) {
        return TRACE_LINK_ERR_ARG;
    }
    depth = s_trace.queue_head - s_trace.queue_tail;
    if (depth >= TRACE_LINK_QUEUE_CAPACITY) {
        ++s_trace.stats.queue_rejected;
        return TRACE_LINK_ERR_FULL;
    }

    frame = &s_trace.queue[s_trace.queue_head & TRACE_LINK_QUEUE_MASK];
    memset(frame->bytes, 0, sizeof(frame->bytes));
    frame->bytes[0] = 'T';
    frame->bytes[1] = 'F';
    frame->bytes[2] = 1u;
    frame->bytes[3] = 1u;
    frame->bytes[4] = source_node;
    frame->bytes[5] = severity;
    TraceLink_WriteU16Le(&frame->bytes[6], event_id);
    TraceLink_WriteU32Le(&frame->bytes[8],
                         s_trace.sequence_by_source[source_node]++);
    TraceLink_WriteU32Le(&frame->bytes[12], timestamp_ms);
    frame->bytes[16] = payload_length;
    if (payload_length > 0u) {
        memcpy(&frame->bytes[17], payload, payload_length);
    }
    crc = TraceLink_Crc32Mpeg2(frame->bytes, 28u);
    if (s_trace.inject_bad_crc_once != 0u) {
        crc ^= 1u;
        s_trace.inject_bad_crc_once = 0u;
        ++s_trace.stats.bad_crc_injected;
    }
    TraceLink_WriteU32Le(&frame->bytes[28], crc);
    __DMB();
    ++s_trace.queue_head;
    ++s_trace.stats.frames_enqueued;
    depth++;
    if (depth > s_trace.stats.queue_high_water) {
        s_trace.stats.queue_high_water = (uint8_t)depth;
    }
    s_trace.stats.queue_pending = (uint8_t)depth;
    return TRACE_LINK_OK;
}

TraceLinkStatus TraceLink_EmitCan(uint8_t source_node,
                                  uint16_t event_id,
                                  uint8_t severity,
                                  uint16_t can_id,
                                  uint8_t dlc,
                                  const uint8_t *data,
                                  uint32_t timestamp_ms)
{
    uint8_t payload[TRACE_LINK_PAYLOAD_CAPACITY] = {0u};

    if ((can_id > 0x7FFu) || (dlc > 8u) || ((dlc > 0u) && (data == 0))) {
        return TRACE_LINK_ERR_ARG;
    }
    TraceLink_WriteU16Le(payload, can_id);
    payload[2] = dlc;
    if (dlc > 0u) {
        memcpy(&payload[3], data, dlc);
    }
    return TraceLink_Emit(source_node, event_id, severity, payload,
                          sizeof(payload), timestamp_ms);
}

void TraceLink_InjectBadCrcOnce(void)
{
    s_trace.inject_bad_crc_once = 1u;
}

void TraceLink_MainFunction(uint32_t now_ms)
{
    uint32_t events;

    if (s_trace.stats.state == TRACE_LINK_STATE_DMA_ACTIVE) {
        events = s_trace.irq_events;
        if ((events & TRACE_LINK_EVENT_ERROR) != 0u) {
            TraceLink_Recover(TRACE_LINK_ERR_HW, now_ms);
        } else if ((events & (TRACE_LINK_EVENT_RX_DONE |
                              TRACE_LINK_EVENT_TX_DONE)) ==
                   (TRACE_LINK_EVENT_RX_DONE | TRACE_LINK_EVENT_TX_DONE)) {
            if ((SPI2->SR & SPI_SR_BSY) == 0u) {
                SPI2->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
                (void)TraceLink_DisableStream(DMA1_Stream3);
                (void)TraceLink_DisableStream(DMA1_Stream4);
                TraceLink_CsHigh();
                TraceLink_ClearSpi();
                ++s_trace.queue_tail;
                ++s_trace.stats.frames_sent;
                s_trace.stats.queue_pending =
                    (uint8_t)(s_trace.queue_head - s_trace.queue_tail);
                s_trace.irq_events = 0u;
                /* Do not reuse a still-high READY level from the transaction
                 * just completed.  One millisecond gives the slave IRQ ample
                 * time to withdraw and later re-grant the credit. */
                s_trace.next_start_ms = now_ms + 1u;
                s_trace.stats.state = TRACE_LINK_STATE_IDLE;
            }
        } else if ((int32_t)(now_ms - s_trace.deadline_ms) >= 0) {
            TraceLink_Recover(TRACE_LINK_ERR_TIMEOUT, now_ms);
        }
    }

    if ((s_trace.stats.state == TRACE_LINK_STATE_IDLE) &&
        (s_trace.queue_tail != s_trace.queue_head) &&
        ((int32_t)(now_ms - s_trace.next_start_ms) >= 0)) {
        (void)TraceLink_StartDma(now_ms);
    }
    s_trace.stats.ready_input = TraceLink_Ready();
}

void TraceLink_GetStats(TraceLinkStats *stats_out)
{
    if (stats_out != 0) {
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        *stats_out = s_trace.stats;
        stats_out->ready_input = TraceLink_Ready();
        stats_out->queue_pending =
            (uint8_t)(s_trace.queue_head - s_trace.queue_tail);
        if (primask == 0u) {
            __enable_irq();
        }
    }
}

void TraceLink_DmaRxIrqHandler(void)
{
    uint32_t flags = DMA1->LISR & TRACE_LINK_RX_FLAG_MASK;

    if (flags != 0u) {
        DMA1->LIFCR = TRACE_LINK_RX_CLEAR_MASK;
        ++s_trace.stats.dma_irq_count;
    }
    if ((flags & TRACE_LINK_RX_ERROR_MASK) != 0u) {
        s_trace.irq_events |= TRACE_LINK_EVENT_ERROR;
    } else if ((flags & DMA_LISR_TCIF3) != 0u) {
        s_trace.irq_events |= TRACE_LINK_EVENT_RX_DONE;
    }
}

void TraceLink_DmaTxIrqHandler(void)
{
    uint32_t flags = DMA1->HISR & TRACE_LINK_TX_FLAG_MASK;

    if (flags != 0u) {
        DMA1->HIFCR = TRACE_LINK_TX_CLEAR_MASK;
        ++s_trace.stats.dma_irq_count;
    }
    if ((flags & TRACE_LINK_TX_ERROR_MASK) != 0u) {
        s_trace.irq_events |= TRACE_LINK_EVENT_ERROR;
    } else if ((flags & DMA_HISR_TCIF4) != 0u) {
        s_trace.irq_events |= TRACE_LINK_EVENT_TX_DONE;
    }
}
