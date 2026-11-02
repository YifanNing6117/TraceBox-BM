#include "main.h"

#include "bringup_app.h"
#include "tracebox_adc_dma.h"
#include "tracebox_board.h"
#include "tracebox_crc.h"
#include "tracebox_nor.h"
#include "tracebox_recorder.h"
#include "tracebox_spi.h"
#include "tracebox_spi_ingress.h"
#include "tracebox_uart.h"

#define TRACEBOX_UART_BAUDRATE 115200U
#define TRACEBOX_SPI_INITIAL_HZ 1312500U
#define TRACEBOX_ADC_SAMPLE_HZ  10000U

static TbStatus SystemClock_Config(void);

int main(void)
{
    uint32_t reset_flags = RCC->CSR;

    HAL_Init();
    TbBoard_Init();

    if (SystemClock_Config() != TB_STATUS_OK)
    {
        TbBoard_FatalBlink(2U);
    }

    SystemCoreClockUpdate();
    TbBoard_DwtInit();

    if (TbCrc_Init() != TB_STATUS_OK)
    {
        TbBoard_FatalBlink(9U);
    }

    if (TbUart_Init(HAL_RCC_GetPCLK2Freq(), TRACEBOX_UART_BAUDRATE) != TB_STATUS_OK)
    {
        TbBoard_FatalBlink(3U);
    }

    if (TbSpi_Init(HAL_RCC_GetPCLK2Freq(), TRACEBOX_SPI_INITIAL_HZ) != TB_STATUS_OK)
    {
        TbBoard_FatalBlink(8U);
    }

    if (TbAdcDma_Init(HAL_RCC_GetPCLK1Freq() * 2U,
                      HAL_RCC_GetPCLK2Freq(),
                      TRACEBOX_ADC_SAMPLE_HZ) != TB_STATUS_OK)
    {
        TbBoard_FatalBlink(10U);
    }

    if (TbSpiIngress_Init() != TB_STATUS_OK)
    {
        TbBoard_FatalBlink(13U);
    }

    if (TbNor_Init() != TB_STATUS_OK)
    {
        TbBoard_FatalBlink(11U);
    }

    if (TbRecorder_Init() != TB_STATUS_OK)
    {
        TbBoard_FatalBlink(12U);
    }

    /* Keep the captured reset cause, then clear the sticky flags for next boot. */
    RCC->CSR |= RCC_CSR_RMVF;

    BringupApp_Init(reset_flags);

    while (1)
    {
        TbSpi_MainFunction();
        TbSpiIngress_MainFunction();
        TbNor_MainFunction(HAL_GetTick());
        TbAdcDma_MainFunction();
        TbRecorder_MainFunction(HAL_GetTick());
        BringupApp_MainFunction(HAL_GetTick());
    }
}

static TbStatus SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 8U;
    osc.PLL.PLLN = 336U;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 7U;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        return TB_STATUS_HW_ERROR;
    }

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK)
    {
        return TB_STATUS_HW_ERROR;
    }

    return TB_STATUS_OK;
}

void Error_Handler(void)
{
    TbBoard_Init();
    TbBoard_FatalBlink(5U);
}
