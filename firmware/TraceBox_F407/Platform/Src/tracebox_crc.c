#include "tracebox_crc.h"

#include "stm32f407xx.h"

TbStatus TbCrc_Init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
    (void)RCC->AHB1ENR;
    CRC->CR = CRC_CR_RESET;
    return TB_STATUS_OK;
}

void TbCrc_Begin(void)
{
    CRC->CR = CRC_CR_RESET;
}

void TbCrc_WriteWord(uint32_t word)
{
    CRC->DR = word;
}

uint32_t TbCrc_Value(void)
{
    return CRC->DR;
}
