#include "tracebox_spi.h"

#include <stdbool.h>

#include "stm32f407xx.h"
#include "tracebox_board.h"

#define TB_SPI_PIN_SCK            5U
#define TB_SPI_PIN_MISO           6U
#define TB_SPI_PIN_MOSI           7U
#define TB_SPI_PIN_CS             0U
#define TB_SPI_PIN_CS_MASK        (1UL << TB_SPI_PIN_CS)
#define TB_SPI_GPIO_AF            5U
#define TB_SPI_ERROR_MASK         (SPI_SR_MODF | SPI_SR_OVR | SPI_SR_CRCERR)
#define TB_SPI_DMA_CHANNEL        3UL
#define TB_SPI_DMA_RX_FLAG_MASK   (DMA_LISR_FEIF2 | DMA_LISR_DMEIF2 | \
                                   DMA_LISR_TEIF2 | DMA_LISR_HTIF2 | \
                                   DMA_LISR_TCIF2)
#define TB_SPI_DMA_TX_FLAG_MASK   (DMA_LISR_FEIF3 | DMA_LISR_DMEIF3 | \
                                   DMA_LISR_TEIF3 | DMA_LISR_HTIF3 | \
                                   DMA_LISR_TCIF3)
#define TB_SPI_DMA_RX_ERROR_MASK  (DMA_LISR_DMEIF2 | DMA_LISR_TEIF2)
#define TB_SPI_DMA_TX_ERROR_MASK  (DMA_LISR_DMEIF3 | DMA_LISR_TEIF3)
#define TB_SPI_DMA_EVENT_RX_DONE  (1UL << 0U)
#define TB_SPI_DMA_EVENT_TX_DONE  (1UL << 1U)
#define TB_SPI_DMA_EVENT_ERROR    (1UL << 2U)
#define TB_SPI_DMA_STOP_SPINS     100000U

typedef struct
{
    TbSpiStats stats;
    volatile uint32_t dma_events;
    volatile uint32_t dma_rx_completed_at;
    uint32_t dma_started_at;
    uint32_t dma_length;
    uint32_t dma_timeout_cycles;
    TbSpiCompletionCallback dma_callback;
    void *dma_user_context;
} TbSpiContext;

static TbSpiContext s_spi;

static void Spi_SetChipSelect(bool selected)
{
    if (selected)
    {
        GPIOB->BSRR = TB_SPI_PIN_CS_MASK << 16U;
    }
    else
    {
        GPIOB->BSRR = TB_SPI_PIN_CS_MASK;
    }
}

static bool Spi_DeadlineExpired(uint32_t started_at, uint32_t timeout_cycles)
{
    return ((uint32_t)(TbBoard_DwtCycles() - started_at) >= timeout_cycles);
}

static void Spi_ClearErrors(void)
{
    volatile uint32_t discard;

    discard = SPI1->DR;
    discard = SPI1->SR;
    (void)discard;
    SPI1->SR &= ~SPI_SR_CRCERR;
}

static void Spi_Recover(void)
{
    uint32_t cr1_configuration = SPI1->CR1 & ~SPI_CR1_SPE;

    Spi_SetChipSelect(false);
    SPI1->CR2 = 0U;
    SPI1->CR1 &= ~SPI_CR1_SPE;

    /* A DMA abort can leave DR/shift state between bytes. Reset SPI1 itself so
       the next transaction cannot consume a stale receive byte. */
    RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
    SPI1->CR1 = cr1_configuration;
    SPI1->CR2 = 0U;
    SPI1->CRCPR = 7U;
    Spi_ClearErrors();
    SPI1->CR1 |= SPI_CR1_SPE;
    ++s_spi.stats.recovery_count;
    s_spi.stats.state = TB_SPI_STATE_READY;
}

static bool Spi_DisableDmaStreams(void)
{
    uint32_t spins = TB_SPI_DMA_STOP_SPINS;

    SPI1->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    DMA2_Stream2->CR &= ~DMA_SxCR_EN;
    DMA2_Stream3->CR &= ~DMA_SxCR_EN;

    while (((DMA2_Stream2->CR & DMA_SxCR_EN) != 0U) ||
           ((DMA2_Stream3->CR & DMA_SxCR_EN) != 0U))
    {
        if (--spins == 0U)
        {
            return false;
        }
    }

    return true;
}

static void Spi_DmaFinish(TbStatus status)
{
    TbSpiCompletionCallback callback = s_spi.dma_callback;
    void *user_context = s_spi.dma_user_context;
    uint32_t elapsed_cycles;

    if ((status == TB_STATUS_OK) && (s_spi.dma_rx_completed_at != 0U))
    {
        elapsed_cycles = s_spi.dma_rx_completed_at - s_spi.dma_started_at;
    }
    else
    {
        elapsed_cycles = TbBoard_DwtCycles() - s_spi.dma_started_at;
    }

    if (!Spi_DisableDmaStreams())
    {
        ++s_spi.stats.hardware_error_count;
        status = TB_STATUS_HW_ERROR;
    }

    Spi_SetChipSelect(false);
    DMA2->LIFCR = TB_SPI_DMA_RX_FLAG_MASK | TB_SPI_DMA_TX_FLAG_MASK;
    NVIC_ClearPendingIRQ(DMA2_Stream2_IRQn);
    NVIC_ClearPendingIRQ(DMA2_Stream3_IRQn);
    __DSB();
    s_spi.stats.dma_last_transfer_cycles = elapsed_cycles;
    if (elapsed_cycles > s_spi.stats.dma_max_transfer_cycles)
    {
        s_spi.stats.dma_max_transfer_cycles = elapsed_cycles;
    }

    if (status == TB_STATUS_OK)
    {
        ++s_spi.stats.dma_transaction_count;
        s_spi.stats.dma_transferred_bytes += s_spi.dma_length;
        s_spi.stats.state = TB_SPI_STATE_READY;
    }
    else
    {
        ++s_spi.stats.dma_error_count;
        Spi_Recover();
    }

    s_spi.dma_callback = 0;
    s_spi.dma_user_context = 0;
    s_spi.dma_events = 0U;

    if (callback != 0)
    {
        callback(status, user_context);
    }
}

static TbStatus Spi_WaitForFlag(uint32_t mask, bool required_set,
                                uint32_t started_at, uint32_t timeout_cycles)
{
    while (((SPI1->SR & mask) != 0U) != required_set)
    {
        if ((SPI1->SR & TB_SPI_ERROR_MASK) != 0U)
        {
            ++s_spi.stats.hardware_error_count;
            return TB_STATUS_HW_ERROR;
        }

        if (Spi_DeadlineExpired(started_at, timeout_cycles))
        {
            ++s_spi.stats.timeout_count;
            return TB_STATUS_TIMEOUT;
        }
    }

    return TB_STATUS_OK;
}

static uint32_t Spi_SelectPrescaler(uint32_t peripheral_clock_hz,
                                   uint32_t maximum_sck_hz,
                                   uint32_t *br_bits_out)
{
    static const uint16_t divisors[8] = {2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U};
    uint32_t index;

    for (index = 0U; index < 8U; ++index)
    {
        if ((peripheral_clock_hz / divisors[index]) <= maximum_sck_hz)
        {
            *br_bits_out = index << SPI_CR1_BR_Pos;
            return divisors[index];
        }
    }

    *br_bits_out = 7UL << SPI_CR1_BR_Pos;
    return 256U;
}

TbStatus TbSpi_Init(uint32_t peripheral_clock_hz, uint32_t maximum_sck_hz)
{
    uint32_t br_bits;
    uint32_t prescaler;
    uint32_t pin;

    if ((peripheral_clock_hz == 0U) || (maximum_sck_hz == 0U) ||
        !TbBoard_DwtAvailable())
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    prescaler = Spi_SelectPrescaler(peripheral_clock_hz, maximum_sck_hz, &br_bits);

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN |
                    RCC_AHB1ENR_DMA2EN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    for (pin = TB_SPI_PIN_SCK; pin <= TB_SPI_PIN_MOSI; ++pin)
    {
        GPIOA->MODER = (GPIOA->MODER & ~(3UL << (pin * 2U))) |
                       (2UL << (pin * 2U));
        GPIOA->OTYPER &= ~(1UL << pin);
        GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3UL << (pin * 2U))) |
                         (2UL << (pin * 2U));
        GPIOA->PUPDR &= ~(3UL << (pin * 2U));
        GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFUL << (pin * 4U))) |
                        (TB_SPI_GPIO_AF << (pin * 4U));
    }

    Spi_SetChipSelect(false);
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << (TB_SPI_PIN_CS * 2U))) |
                   (1UL << (TB_SPI_PIN_CS * 2U));
    GPIOB->OTYPER &= ~TB_SPI_PIN_CS_MASK;
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR & ~(3UL << (TB_SPI_PIN_CS * 2U))) |
                     (2UL << (TB_SPI_PIN_CS * 2U));
    GPIOB->PUPDR &= ~(3UL << (TB_SPI_PIN_CS * 2U));

    RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;

    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | br_bits;
    SPI1->CR2 = 0U;
    SPI1->CRCPR = 7U;
    Spi_ClearErrors();
    SPI1->CR1 |= SPI_CR1_SPE;

    DMA2_Stream2->CR = 0U;
    DMA2_Stream2->FCR = 0U;
    DMA2_Stream3->CR = 0U;
    DMA2_Stream3->FCR = 0U;
    DMA2->LIFCR = TB_SPI_DMA_RX_FLAG_MASK | TB_SPI_DMA_TX_FLAG_MASK;

    NVIC_SetPriority(DMA2_Stream2_IRQn, 3U);
    NVIC_ClearPendingIRQ(DMA2_Stream2_IRQn);
    NVIC_EnableIRQ(DMA2_Stream2_IRQn);
    NVIC_SetPriority(DMA2_Stream3_IRQn, 3U);
    NVIC_ClearPendingIRQ(DMA2_Stream3_IRQn);
    NVIC_EnableIRQ(DMA2_Stream3_IRQn);

    s_spi.stats.state = TB_SPI_STATE_READY;
    s_spi.stats.peripheral_clock_hz = peripheral_clock_hz;
    s_spi.stats.prescaler = prescaler;
    s_spi.stats.sck_hz = peripheral_clock_hz / prescaler;
    s_spi.stats.polling_transaction_count = 0U;
    s_spi.stats.polling_transferred_bytes = 0U;
    s_spi.stats.dma_transaction_count = 0U;
    s_spi.stats.dma_transferred_bytes = 0U;
    s_spi.stats.dma_irq_count = 0U;
    s_spi.stats.dma_error_count = 0U;
    s_spi.stats.timeout_count = 0U;
    s_spi.stats.hardware_error_count = 0U;
    s_spi.stats.recovery_count = 0U;
    s_spi.stats.last_transfer_cycles = 0U;
    s_spi.stats.max_transfer_cycles = 0U;
    s_spi.stats.dma_last_transfer_cycles = 0U;
    s_spi.stats.dma_max_transfer_cycles = 0U;
    s_spi.dma_events = 0U;
    s_spi.dma_rx_completed_at = 0U;
    s_spi.dma_started_at = 0U;
    s_spi.dma_length = 0U;
    s_spi.dma_timeout_cycles = 0U;
    s_spi.dma_callback = 0;
    s_spi.dma_user_context = 0;

    return TB_STATUS_OK;
}

TbStatus TbSpi_TransferPolling(const TbSpiTransfer *transfer)
{
    TbStatus status = TB_STATUS_OK;
    uint32_t index;
    uint32_t started_at;

    if ((transfer == 0) || (transfer->length == 0U) ||
        (transfer->length > TB_SPI_MAX_TRANSFER_LENGTH) ||
        (transfer->timeout_cycles == 0U) ||
        ((transfer->tx_data == 0) && (transfer->rx_data == 0)))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    if (s_spi.stats.state != TB_SPI_STATE_READY)
    {
        return TB_STATUS_BUSY;
    }

    s_spi.stats.state = TB_SPI_STATE_POLLING;
    started_at = TbBoard_DwtCycles();
    Spi_SetChipSelect(true);

    for (index = 0U; index < transfer->length; ++index)
    {
        uint8_t tx_byte = (transfer->tx_data != 0) ? transfer->tx_data[index] : 0xFFU;
        uint8_t rx_byte;

        status = Spi_WaitForFlag(SPI_SR_TXE, true, started_at,
                                 transfer->timeout_cycles);
        if (status != TB_STATUS_OK)
        {
            break;
        }

        *(__IO uint8_t *)&SPI1->DR = tx_byte;

        status = Spi_WaitForFlag(SPI_SR_RXNE, true, started_at,
                                 transfer->timeout_cycles);
        if (status != TB_STATUS_OK)
        {
            break;
        }

        rx_byte = *(__IO uint8_t *)&SPI1->DR;
        if (transfer->rx_data != 0)
        {
            transfer->rx_data[index] = rx_byte;
        }
    }

    if (status == TB_STATUS_OK)
    {
        status = Spi_WaitForFlag(SPI_SR_BSY, false, started_at,
                                 transfer->timeout_cycles);
    }

    s_spi.stats.last_transfer_cycles = TbBoard_DwtCycles() - started_at;
    if (s_spi.stats.last_transfer_cycles > s_spi.stats.max_transfer_cycles)
    {
        s_spi.stats.max_transfer_cycles = s_spi.stats.last_transfer_cycles;
    }

    if (status == TB_STATUS_OK)
    {
        Spi_SetChipSelect(false);
        ++s_spi.stats.polling_transaction_count;
        s_spi.stats.polling_transferred_bytes += transfer->length;
        s_spi.stats.state = TB_SPI_STATE_READY;
    }
    else
    {
        Spi_Recover();
    }

    return status;
}

TbStatus TbSpi_TransferDmaStart(const TbSpiDmaTransfer *transfer)
{
    uint32_t started_at;

    if ((transfer == 0) || (transfer->tx_data == 0) ||
        (transfer->rx_data == 0) || (transfer->length == 0U) ||
        (transfer->length > TB_SPI_MAX_TRANSFER_LENGTH) ||
        (transfer->timeout_cycles == 0U))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    if (s_spi.stats.state != TB_SPI_STATE_READY)
    {
        return TB_STATUS_BUSY;
    }

    if (!Spi_DisableDmaStreams())
    {
        ++s_spi.stats.hardware_error_count;
        return TB_STATUS_HW_ERROR;
    }

    DMA2->LIFCR = TB_SPI_DMA_RX_FLAG_MASK | TB_SPI_DMA_TX_FLAG_MASK;
    NVIC_ClearPendingIRQ(DMA2_Stream2_IRQn);
    NVIC_ClearPendingIRQ(DMA2_Stream3_IRQn);
    s_spi.dma_events = 0U;
    s_spi.dma_rx_completed_at = 0U;
    s_spi.dma_length = transfer->length;
    s_spi.dma_timeout_cycles = transfer->timeout_cycles;
    s_spi.dma_callback = transfer->completion_callback;
    s_spi.dma_user_context = transfer->user_context;

    DMA2_Stream2->PAR = (uint32_t)&SPI1->DR;
    DMA2_Stream2->M0AR = (uint32_t)transfer->rx_data;
    DMA2_Stream2->NDTR = transfer->length;
    DMA2_Stream2->FCR = 0U;
    DMA2_Stream2->CR = (TB_SPI_DMA_CHANNEL << DMA_SxCR_CHSEL_Pos) |
                       DMA_SxCR_PL_1 | DMA_SxCR_MINC | DMA_SxCR_TCIE |
                       DMA_SxCR_TEIE | DMA_SxCR_DMEIE;

    DMA2_Stream3->PAR = (uint32_t)&SPI1->DR;
    DMA2_Stream3->M0AR = (uint32_t)transfer->tx_data;
    DMA2_Stream3->NDTR = transfer->length;
    DMA2_Stream3->FCR = 0U;
    DMA2_Stream3->CR = (TB_SPI_DMA_CHANNEL << DMA_SxCR_CHSEL_Pos) |
                       DMA_SxCR_PL_1 | DMA_SxCR_MINC | DMA_SxCR_DIR_0 |
                       DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;

    Spi_SetChipSelect(true);
    started_at = TbBoard_DwtCycles();
    s_spi.dma_started_at = started_at;
    s_spi.stats.state = TB_SPI_STATE_DMA_ACTIVE;
    __DMB();

    SPI1->CR2 |= SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    DMA2_Stream2->CR |= DMA_SxCR_EN;
    DMA2_Stream3->CR |= DMA_SxCR_EN;

    return TB_STATUS_OK;
}

void TbSpi_MainFunction(void)
{
    uint32_t events;

    if (s_spi.stats.state != TB_SPI_STATE_DMA_ACTIVE)
    {
        return;
    }

    events = s_spi.dma_events;
    if ((events & TB_SPI_DMA_EVENT_ERROR) != 0U)
    {
        ++s_spi.stats.hardware_error_count;
        Spi_DmaFinish(TB_STATUS_HW_ERROR);
        return;
    }

    if ((SPI1->SR & TB_SPI_ERROR_MASK) != 0U)
    {
        ++s_spi.stats.hardware_error_count;
        Spi_DmaFinish(TB_STATUS_HW_ERROR);
        return;
    }

    if ((events & (TB_SPI_DMA_EVENT_RX_DONE | TB_SPI_DMA_EVENT_TX_DONE)) ==
        (TB_SPI_DMA_EVENT_RX_DONE | TB_SPI_DMA_EVENT_TX_DONE))
    {
        if ((SPI1->SR & SPI_SR_BSY) == 0U)
        {
            Spi_DmaFinish(TB_STATUS_OK);
            return;
        }
    }

    if (Spi_DeadlineExpired(s_spi.dma_started_at, s_spi.dma_timeout_cycles))
    {
        ++s_spi.stats.timeout_count;
        Spi_DmaFinish(TB_STATUS_TIMEOUT);
    }
}

void TbSpi_DmaRxIrqHandler(void)
{
    uint32_t flags = DMA2->LISR & TB_SPI_DMA_RX_FLAG_MASK;

    if (flags == 0U)
    {
        return;
    }

    DMA2->LIFCR = flags;
    ++s_spi.stats.dma_irq_count;

    if ((flags & TB_SPI_DMA_RX_ERROR_MASK) != 0U)
    {
        s_spi.dma_events |= TB_SPI_DMA_EVENT_ERROR;
    }
    if ((flags & DMA_LISR_TCIF2) != 0U)
    {
        s_spi.dma_rx_completed_at = TbBoard_DwtCycles();
        s_spi.dma_events |= TB_SPI_DMA_EVENT_RX_DONE;
    }
}

void TbSpi_DmaTxIrqHandler(void)
{
    uint32_t flags = DMA2->LISR & TB_SPI_DMA_TX_FLAG_MASK;

    if (flags == 0U)
    {
        return;
    }

    DMA2->LIFCR = flags;
    ++s_spi.stats.dma_irq_count;

    if ((flags & TB_SPI_DMA_TX_ERROR_MASK) != 0U)
    {
        s_spi.dma_events |= TB_SPI_DMA_EVENT_ERROR;
    }
    if ((flags & DMA_LISR_TCIF3) != 0U)
    {
        s_spi.dma_events |= TB_SPI_DMA_EVENT_TX_DONE;
    }
}

void TbSpi_GetStats(TbSpiStats *stats_out)
{
    if (stats_out != 0)
    {
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        *stats_out = s_spi.stats;
        if (primask == 0U)
        {
            __enable_irq();
        }
    }
}
