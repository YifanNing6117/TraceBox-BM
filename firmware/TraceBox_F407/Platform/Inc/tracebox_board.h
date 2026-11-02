#ifndef TRACEBOX_BOARD_H
#define TRACEBOX_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "tracebox_types.h"

typedef enum
{
    TB_LED_OFF = 0,
    TB_LED_ON = 1
} TbLedState;

void TbBoard_Init(void);
void TbBoard_SetLed(TbLedState state);
void TbBoard_ToggleLed(void);
TbLedState TbBoard_GetLed(void);

void TbBoard_DwtInit(void);
bool TbBoard_DwtAvailable(void);
uint32_t TbBoard_DwtCycles(void);

void TbBoard_FatalBlink(uint32_t pulse_count);

#endif /* TRACEBOX_BOARD_H */
