#ifndef TRACEBOX_SPI_H
#define TRACEBOX_SPI_H

#include <stdint.h>

#include "tracebox_types.h"

#define TB_SPI_MAX_TRANSFER_LENGTH 4096U

typedef enum
{
    TB_SPI_STATE_RESET = 0,
    TB_SPI_STATE_READY,
    TB_SPI_STATE_POLLING,
    TB_SPI_STATE_DMA_ACTIVE
} TbSpiState;

typedef struct
{
    const uint8_t *tx_data;
    uint8_t *rx_data;
    uint32_t length;
    uint32_t timeout_cycles;
} TbSpiTransfer;

typedef void (*TbSpiCompletionCallback)(TbStatus status, void *user_context);

typedef struct
{
    const uint8_t *tx_data;
    uint8_t *rx_data;
    uint32_t length;
    uint32_t timeout_cycles;
    TbSpiCompletionCallback completion_callback;
    void *user_context;
} TbSpiDmaTransfer;

typedef struct
{
    TbSpiState state;
    uint32_t peripheral_clock_hz;
    uint32_t sck_hz;
    uint32_t prescaler;
    uint32_t polling_transaction_count;
    uint32_t polling_transferred_bytes;
    uint32_t dma_transaction_count;
    uint32_t dma_transferred_bytes;
    uint32_t dma_irq_count;
    uint32_t dma_error_count;
    uint32_t timeout_count;
    uint32_t hardware_error_count;
    uint32_t recovery_count;
    uint32_t last_transfer_cycles;
    uint32_t max_transfer_cycles;
    uint32_t dma_last_transfer_cycles;
    uint32_t dma_max_transfer_cycles;
} TbSpiStats;

TbStatus TbSpi_Init(uint32_t peripheral_clock_hz, uint32_t maximum_sck_hz);
TbStatus TbSpi_TransferPolling(const TbSpiTransfer *transfer);
TbStatus TbSpi_TransferDmaStart(const TbSpiDmaTransfer *transfer);
void TbSpi_MainFunction(void);
void TbSpi_GetStats(TbSpiStats *stats_out);

/* IRQ entry points only clear and publish DMA flags; completion stays foreground. */
void TbSpi_DmaRxIrqHandler(void);
void TbSpi_DmaTxIrqHandler(void);

#endif /* TRACEBOX_SPI_H */
