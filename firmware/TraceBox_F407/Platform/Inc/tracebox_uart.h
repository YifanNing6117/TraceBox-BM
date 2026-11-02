#ifndef TRACEBOX_UART_H
#define TRACEBOX_UART_H

#include <stdbool.h>
#include <stdint.h>

#include "tracebox_types.h"

typedef struct
{
    uint32_t baudrate;
    uint32_t baud_switch_count;
    uint32_t irq_count;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_overflow_count;
    uint32_t peripheral_error_count;
    uint32_t tx_timeout_count;
    uint16_t rx_high_water;
    uint16_t rx_pending;
} TbUartStats;

TbStatus TbUart_Init(uint32_t peripheral_clock_hz, uint32_t baudrate);
TbStatus TbUart_SetBaudrate(uint32_t baudrate);
TbStatus TbUart_Write(const uint8_t *data, uint16_t length);
TbStatus TbUart_WriteString(const char *text);
TbStatus TbUart_WaitTxComplete(void);
bool TbUart_ReadByte(uint8_t *byte_out);
void TbUart_GetStats(TbUartStats *stats_out);

/* Called only by USART1_IRQHandler; it publishes bytes and counters, nothing more. */
void TbUart_IrqHandler(void);

#endif /* TRACEBOX_UART_H */
