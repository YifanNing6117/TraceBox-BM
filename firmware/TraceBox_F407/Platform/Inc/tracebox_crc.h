#ifndef TRACEBOX_CRC_H
#define TRACEBOX_CRC_H

#include <stdint.h>

#include "tracebox_types.h"

TbStatus TbCrc_Init(void);
void TbCrc_Begin(void);
void TbCrc_WriteWord(uint32_t word);
uint32_t TbCrc_Value(void);

#endif /* TRACEBOX_CRC_H */
