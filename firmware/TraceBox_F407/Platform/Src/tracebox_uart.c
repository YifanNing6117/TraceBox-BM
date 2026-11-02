#include "tracebox_uart.h"

#include <stddef.h>

#include "stm32f407xx.h"

#define TB_UART_RX_CAPACITY       128U
#define TB_UART_RX_MASK           (TB_UART_RX_CAPACITY - 1U)
#define TB_UART_TX_SPIN_LIMIT     1000000U
#define TB_UART_ERROR_MASK        (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)

static volatile uint8_t s_rx_buffer[TB_UART_RX_CAPACITY];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static volatile TbUartStats s_stats;
static uint32_t s_peripheral_clock_hz;

static TbStatus TbUart_CalculateBrr(uint32_t peripheral_clock_hz,
                                    uint32_t baudrate,
                                    uint32_t *brr_out)
{
    uint32_t brr;

    if ((peripheral_clock_hz == 0U) || (baudrate == 0U) ||
        (brr_out == NULL))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    brr = (peripheral_clock_hz + (baudrate / 2U)) / baudrate;
    if ((brr < 16U) || (brr > 0xFFFFU))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }
    *brr_out = brr;
    return TB_STATUS_OK;
}

static uint16_t TbUart_PendingUnsafe(void)
{
    return (uint16_t)((s_rx_head - s_rx_tail) & TB_UART_RX_MASK);
}

TbStatus TbUart_Init(uint32_t peripheral_clock_hz, uint32_t baudrate)
{
    uint32_t brr;

    if (TbUart_CalculateBrr(peripheral_clock_hz, baudrate, &brr) !=
        TB_STATUS_OK)
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;

    /* PA9=USART1_TX and PA10=USART1_RX, alternate function 7. */
    GPIOA->MODER = (GPIOA->MODER & ~((3UL << 18U) | (3UL << 20U))) |
                   (2UL << 18U) | (2UL << 20U);
    GPIOA->OTYPER &= ~((1UL << 9U) | (1UL << 10U));
    GPIOA->OSPEEDR |= (3UL << 18U) | (3UL << 20U);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3UL << 18U) | (3UL << 20U))) |
                   (1UL << 18U) | (1UL << 20U);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFUL << 4U) | (0xFUL << 8U))) |
                    (7UL << 4U) | (7UL << 8U);

    RCC->APB2RSTR |= RCC_APB2RSTR_USART1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_USART1RST;

    USART1->CR1 = 0U;
    USART1->CR2 = 0U;
    USART1->CR3 = 0U;
    USART1->BRR = brr;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    s_peripheral_clock_hz = peripheral_clock_hz;
    s_stats.baudrate = baudrate;

    NVIC_SetPriority(USART1_IRQn, 5U);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);

    return TB_STATUS_OK;
}

TbStatus TbUart_WaitTxComplete(void)
{
    uint32_t spins = TB_UART_TX_SPIN_LIMIT;

    while ((USART1->SR & USART_SR_TC) == 0U)
    {
        if (--spins == 0U)
        {
            ++s_stats.tx_timeout_count;
            return TB_STATUS_TIMEOUT;
        }
    }
    return TB_STATUS_OK;
}

TbStatus TbUart_SetBaudrate(uint32_t baudrate)
{
    uint32_t brr;
    TbStatus status;

    status = TbUart_CalculateBrr(s_peripheral_clock_hz, baudrate, &brr);
    if (status != TB_STATUS_OK)
    {
        return status;
    }
    status = TbUart_WaitTxComplete();
    if (status != TB_STATUS_OK)
    {
        return status;
    }

    USART1->CR1 &= ~USART_CR1_UE;
    USART1->BRR = brr;
    USART1->CR1 |= USART_CR1_UE;
    s_stats.baudrate = baudrate;
    ++s_stats.baud_switch_count;
    return TB_STATUS_OK;
}

TbStatus TbUart_Write(const uint8_t *data, uint16_t length)
{
    uint16_t index;

    if ((data == NULL) && (length != 0U))
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0U; index < length; ++index)
    {
        uint32_t spins = TB_UART_TX_SPIN_LIMIT;

        while ((USART1->SR & USART_SR_TXE) == 0U)
        {
            if (--spins == 0U)
            {
                ++s_stats.tx_timeout_count;
                return TB_STATUS_TIMEOUT;
            }
        }

        USART1->DR = data[index];
        ++s_stats.tx_bytes;
    }

    return TB_STATUS_OK;
}

TbStatus TbUart_WriteString(const char *text)
{
    uint16_t length = 0U;

    if (text == NULL)
    {
        return TB_STATUS_INVALID_ARGUMENT;
    }

    while (text[length] != '\0')
    {
        if (length == 0xFFFFU)
        {
            return TB_STATUS_OVERFLOW;
        }
        ++length;
    }

    return TbUart_Write((const uint8_t *)text, length);
}

bool TbUart_ReadByte(uint8_t *byte_out)
{
    if ((byte_out == NULL) || (s_rx_tail == s_rx_head))
    {
        return false;
    }

    *byte_out = s_rx_buffer[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) & TB_UART_RX_MASK);
    return true;
}

void TbUart_GetStats(TbUartStats *stats_out)
{
    uint32_t primask;

    if (stats_out == NULL)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    *stats_out = s_stats;
    stats_out->rx_pending = TbUart_PendingUnsafe();
    if (primask == 0U)
    {
        __enable_irq();
    }
}

void TbUart_IrqHandler(void)
{
    uint32_t status = USART1->SR;

    ++s_stats.irq_count;

    if ((status & (USART_SR_RXNE | TB_UART_ERROR_MASK)) != 0U)
    {
        uint8_t byte = (uint8_t)USART1->DR;

        if ((status & TB_UART_ERROR_MASK) != 0U)
        {
            ++s_stats.peripheral_error_count;
        }

        if ((status & USART_SR_RXNE) != 0U)
        {
            uint16_t next = (uint16_t)((s_rx_head + 1U) & TB_UART_RX_MASK);

            ++s_stats.rx_bytes;
            if (next == s_rx_tail)
            {
                ++s_stats.rx_overflow_count;
            }
            else
            {
                uint16_t pending;

                s_rx_buffer[s_rx_head] = byte;
                s_rx_head = next;
                pending = TbUart_PendingUnsafe();
                if (pending > s_stats.rx_high_water)
                {
                    s_stats.rx_high_water = pending;
                }
            }
        }
    }
}
