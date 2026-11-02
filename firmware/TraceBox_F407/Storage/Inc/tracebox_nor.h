#ifndef TRACEBOX_NOR_H
#define TRACEBOX_NOR_H

#include <stdint.h>

#include "tracebox_types.h"

#define TB_NOR_PAGE_SIZE_BYTES    256U
#define TB_NOR_SECTOR_SIZE_BYTES  4096U

typedef enum
{
    TB_NOR_STATE_RESET = 0,
    TB_NOR_STATE_READY,
    TB_NOR_STATE_BUSY,
    TB_NOR_STATE_FAULT
} TbNorState;

typedef void (*TbNorCompletionCallback)(TbStatus status, void *user_context);

typedef struct
{
    TbNorState state;
    uint8_t manufacturer_id;
    uint8_t memory_type;
    uint8_t capacity_code;
    uint8_t status_register_1;
    uint32_t capacity_bytes;
    uint32_t identify_count;
    uint32_t sector_erase_count;
    uint32_t page_program_count;
    uint32_t read_count;
    uint32_t programmed_bytes;
    uint32_t read_bytes;
    uint32_t status_poll_count;
    uint32_t spi_error_count;
    uint32_t timeout_count;
    uint32_t write_enable_error_count;
    uint32_t recovery_count;
} TbNorStats;

TbStatus TbNor_Init(void);
TbStatus TbNor_IdentifyStart(TbNorCompletionCallback callback,
                             void *user_context);
TbStatus TbNor_SectorEraseStart(uint32_t address,
                                uint32_t timeout_ms,
                                TbNorCompletionCallback callback,
                                void *user_context);
TbStatus TbNor_PageProgramStart(uint32_t address,
                                const uint8_t *data,
                                uint16_t length,
                                uint32_t timeout_ms,
                                TbNorCompletionCallback callback,
                                void *user_context);
TbStatus TbNor_ReadStart(uint32_t address,
                         uint8_t *data_out,
                         uint16_t length,
                         TbNorCompletionCallback callback,
                         void *user_context);
TbStatus TbNor_RecoverStart(uint32_t timeout_ms,
                            TbNorCompletionCallback callback,
                            void *user_context);
void TbNor_MainFunction(uint32_t now_ms);
void TbNor_GetStats(TbNorStats *stats_out);

#endif /* TRACEBOX_NOR_H */
