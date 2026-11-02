#include "tracebox_board.h"

#include "stm32f407xx.h"

#define TB_LED_PIN             0U
#define TB_LED_MASK            (1UL << TB_LED_PIN)
#define TB_FATAL_DELAY_LOOPS   650000U

static bool s_dwt_available;

void TbBoard_Init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    (void)RCC->AHB1ENR;

    GPIOC->MODER = (GPIOC->MODER & ~(3UL << (TB_LED_PIN * 2U))) |
                   (1UL << (TB_LED_PIN * 2U));
    GPIOC->OTYPER &= ~TB_LED_MASK;
    GPIOC->OSPEEDR &= ~(3UL << (TB_LED_PIN * 2U));
    GPIOC->PUPDR &= ~(3UL << (TB_LED_PIN * 2U));

    TbBoard_SetLed(TB_LED_OFF);
}

void TbBoard_SetLed(TbLedState state)
{
    if (state == TB_LED_ON)
    {
        /* LXB407VE-P1 LED is active-low: reset PC0 to turn it on. */
        GPIOC->BSRR = (TB_LED_MASK << 16U);
    }
    else
    {
        GPIOC->BSRR = TB_LED_MASK;
    }
}

void TbBoard_ToggleLed(void)
{
    GPIOC->ODR ^= TB_LED_MASK;
}

TbLedState TbBoard_GetLed(void)
{
    return ((GPIOC->ODR & TB_LED_MASK) == 0U) ? TB_LED_ON : TB_LED_OFF;
}

void TbBoard_DwtInit(void)
{
    uint32_t before;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    before = DWT->CYCCNT;
    __NOP();
    __NOP();
    __NOP();
    s_dwt_available = (DWT->CYCCNT != before);
}

bool TbBoard_DwtAvailable(void)
{
    return s_dwt_available;
}

uint32_t TbBoard_DwtCycles(void)
{
    return DWT->CYCCNT;
}

void TbBoard_FatalBlink(uint32_t pulse_count)
{
    volatile uint32_t delay;
    uint32_t pulse;

    if (pulse_count == 0U)
    {
        pulse_count = 1U;
    }

    __disable_irq();
    TbBoard_Init();

    while (1)
    {
        for (pulse = 0U; pulse < pulse_count; ++pulse)
        {
            TbBoard_SetLed(TB_LED_ON);
            for (delay = 0U; delay < TB_FATAL_DELAY_LOOPS; ++delay)
            {
                __NOP();
            }
            TbBoard_SetLed(TB_LED_OFF);
            for (delay = 0U; delay < TB_FATAL_DELAY_LOOPS; ++delay)
            {
                __NOP();
            }
        }

        for (delay = 0U; delay < (TB_FATAL_DELAY_LOOPS * 4U); ++delay)
        {
            __NOP();
        }
    }
}
