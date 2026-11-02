#include "main.h"
#include "stm32f4xx_it.h"

#include "tracebox_board.h"
#include "tracebox_adc_dma.h"
#include "tracebox_spi.h"
#include "tracebox_spi_ingress.h"
#include "tracebox_uart.h"

void NMI_Handler(void)
{
    TbBoard_FatalBlink(2U);
}

void HardFault_Handler(void)
{
    TbBoard_FatalBlink(4U);
}

void MemManage_Handler(void)
{
    TbBoard_FatalBlink(5U);
}

void BusFault_Handler(void)
{
    TbBoard_FatalBlink(6U);
}

void UsageFault_Handler(void)
{
    TbBoard_FatalBlink(7U);
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void USART1_IRQHandler(void)
{
    TbUart_IrqHandler();
}

void DMA2_Stream2_IRQHandler(void)
{
    TbSpi_DmaRxIrqHandler();
}

void DMA2_Stream3_IRQHandler(void)
{
    TbSpi_DmaTxIrqHandler();
}

void DMA2_Stream4_IRQHandler(void)
{
    TbAdcDma_IrqHandler();
}

void DMA1_Stream3_IRQHandler(void)
{
    TbSpiIngress_DmaRxIrqHandler();
}

void SPI2_IRQHandler(void)
{
    TbSpiIngress_SpiIrqHandler();
}

void EXTI15_10_IRQHandler(void)
{
    if ((EXTI->PR & EXTI_PR_PR12) != 0U)
    {
        EXTI->PR = EXTI_PR_PR12;
        TbSpiIngress_NssRisingIrqHandler();
    }
}
