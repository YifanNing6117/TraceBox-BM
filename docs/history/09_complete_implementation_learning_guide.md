> 历史设计记录：原开发目录中的路径、阶段目标与实验记录按当时状态保留。当前构建、运行和验证入口见[仓库 README](../../README.md)。部分原始日志与规划资料未纳入公开快照。

# TraceBox-BM 完整实现与学习指南

> 本文从“为什么这样做”讲到“代码怎样运行”，适合整理简历、复习源码和准备面试。
> 文中的 **MEASURED** 是已经归档的实机结果；**DESIGN** 是设计事实；**V2 PLAN** 是尚待
> 三板实测的扩展，三者不能混写。

## 1. 用一句话理解项目

TraceBox-BM 是一台独立的 MCU 数据黑匣子：它不用 RTOS，而是让 STM32F407 的 Timer、ADC、
DMA 和 SPI 硬件协作，连续取得数据；软件用固定 RAM 页池吸收 Flash 延迟，用异步状态机驱动
W25Q64，将数据写成带 CRC 和提交标记的 256 字节页；重启后只接受连续、完整、校验正确的页，
最后由 Python 和 localhost 页面导出、校验并形成证据。

## 2. 它解决的不是“读到数据”，而是六个系统问题

普通课程实验通常只证明“调用 SPI API 后读到了器件 ID”。本项目继续追问：

1. 数据能否按固定周期产生，而不是被主循环快慢影响？
2. CPU 不逐点搬运时，如何知道哪一块 DMA 内存刚完成？
3. Flash 某一页暂时写得慢时，下一块采样数据放在哪里？
4. 写到一半断电时，如何区别完整历史与残缺尾页？
5. DMA 或 Flash 一直 busy 时，系统怎样有界退出并恢复下一笔事务？
6. “没有丢数据”如何被 PC 独立验证，而不是只看 MCU 打印一句 PASS？

这六个问题对应本项目的六条主线：

```text
确定性产生    TIM2 TRGO -> ADC1
硬件搬运      ADC DMA DBM / SPI TX+RX DMA
速率解耦      descriptor ring + fixed page pool
一致性提交    page CRC + body/commit two-phase program
异常恢复      deadline + typed error + peripheral reset/recovery scan
证据闭环      DWT metrics + binary dump + independent PC codec
```

## 3. 硬件与资源分配

### 3.1 核心硬件

| 器件 | 用途 | 关键点 |
|---|---|---|
| LXB407VE-P1 / STM32F407VET6 | 主控 | Cortex-M4，168 MHz，512 KiB Flash，足够的 DMA Stream |
| W25Q64 | 8 MiB SPI NOR | 256 B page program、4 KiB sector erase、JEDEC `EF 40 17` |
| 10 kΩ 电位器模块 | ADC 实际激励 | 两端接 3V3/GND，中点 OUT 接 PA0 |
| ST-Link | SWD 下载和调试 | SWDIO、SWCLK、NRST、GND |
| USB-UART | 控制和 dump | USART1，115200 控制、460800 二进制导出 |
| 24 MHz/8CH 逻辑分析仪 | SPI 时序证据 | 当前缺连接线，因此功能已闭环但波形截图待补 |

### 3.2 固定引脚

| 功能 | 引脚 | 寄存器配置 |
|---|---|---|
| LED | PC0 | GPIO output，板载灯 active-low |
| USART1 TX/RX | PA9/PA10 | AF7 |
| ADC1_IN0 | PA0 | Analog，无上下拉 |
| SPI1 SCK/MISO/MOSI | PA5/PA6/PA7 | AF5 |
| W25Q64 CS# | PB0 | GPIO output，`BSRR` 原子拉低/拉高 |

W25Q64 只接一颗，供电必须为 3.3 V。V1 的一周闭环不需要 MCP3208、显示屏、SD 卡或第二颗
Flash。

### 3.3 DMA Stream 规划

| 外设请求 | DMA/Stream/Channel | 模式 |
|---|---|---|
| ADC1 | DMA2 Stream4 Channel0 | P→M、halfword、circular、double buffer |
| SPI1_RX | DMA2 Stream2 Channel3 | P→M、byte、normal |
| SPI1_TX | DMA2 Stream3 Channel3 | M→P、byte、normal |

三条链路不共享 Stream。SPI 全双工即便只关心读数据，也必须持续发送 dummy byte 才能产生
时钟，因此 RX 和 TX DMA 成对工作。

## 4. 软件分层与目录职责

```text
Core/Src/main.c
  初始化 + 裸机超级循环

App/
  UART 命令、Demo 编排、dump 状态机、fault injection

Recorder/
  组合 ADC producer、page pool 和 NOR consumer

Storage/
  W25Q64 命令语义与 WEL/WIP/deadline 状态机

TraceCore/
  event ring、page pool、固定页 codec

Platform/
  board/DWT、UART、CRC、ADC DMA、SPI DMA 寄存器驱动

tools/
  Python 串口唯一 owner、二进制 dump、独立 codec、CSV/JSON/Markdown

web-console/
  localhost HTTP 编排与中文界面，不重复串口/CRC 逻辑
```

依赖方向只允许从上向下。Recorder 可以调用 ADC、PagePool、PageCodec 和 NOR，但 NOR 不知道
ADC，SPI 也不知道 W25Q64 命令。这一点使每层能够单独 loopback/selftest，也使故障能够定位。

## 5. 启动流程和裸机执行模型

### 5.1 `main()` 做了什么

入口在 `firmware/TraceBox_F407/Core/Src/main.c`。执行顺序是：

1. 在清除之前读取 `RCC->CSR`，保存复位原因；
2. `HAL_Init()` 提供基础 HAL tick；
3. 初始化 PC0 LED；
4. 用 HSE 8 MHz + PLL 配置 SYSCLK 168 MHz；
5. 启用 DWT `CYCCNT`；
6. 初始化硬件 CRC、USART1、SPI1、ADC DMA、NOR 和 Recorder；
7. 清除粘滞复位标志；
8. 输出 banner，并自动启动只读恢复扫描；
9. 永久运行合作式超级循环。

真实代码骨架：

```c
int main(void)
{
    uint32_t reset_flags = RCC->CSR;

    HAL_Init();
    TbBoard_Init();
    SystemClock_Config();
    SystemCoreClockUpdate();
    TbBoard_DwtInit();

    TbCrc_Init();
    TbUart_Init(HAL_RCC_GetPCLK2Freq(), 115200U);
    TbSpi_Init(HAL_RCC_GetPCLK2Freq(), 1312500U);
    TbAdcDma_Init(HAL_RCC_GetPCLK1Freq() * 2U,
                  HAL_RCC_GetPCLK2Freq(), 10000U);
    TbNor_Init();
    TbRecorder_Init();

    RCC->CSR |= RCC_CSR_RMVF;
    BringupApp_Init(reset_flags);

    while (1) {
        TbSpi_MainFunction();
        TbNor_MainFunction(HAL_GetTick());
        TbAdcDma_MainFunction();
        TbRecorder_MainFunction(HAL_GetTick());
        BringupApp_MainFunction(HAL_GetTick());
    }
}
```

生产代码逐个检查返回值，并用不同 LED 脉冲数表示初始化阶段的 fatal error；上面的片段省略了
这些重复判断，只突出控制流。

### 5.2 为什么不需要 RTOS

裸机不等于所有事情都写在一个 `while(1)` 中。这里使用的是“事件 + 短状态机”调度：

- ISR 发布完成/错误元数据；
- 每个 `MainFunction` 检查自己状态；
- 能推进一步就推进一步，不能推进就立即返回；
- 等待 DMA/WIP 的含义由状态和 deadline 表示，而不是 while busy；
- callback 只在前台状态机中调用。

这相当于把任务阻塞点显式化。面试时可以说：RTOS 把等待隐藏在线程状态中；本项目为了展示
底层控制，将等待表示成显式 FSM，因此每次主循环执行时间有界。

### 5.3 中断入口

`Core/Src/stm32f4xx_it.c` 只有路由：

```c
void DMA2_Stream2_IRQHandler(void) { TbSpi_DmaRxIrqHandler(); }
void DMA2_Stream3_IRQHandler(void) { TbSpi_DmaTxIrqHandler(); }
void DMA2_Stream4_IRQHandler(void) { TbAdcDma_IrqHandler(); }
void USART1_IRQHandler(void)       { TbUart_IrqHandler(); }
```

ISR 禁止执行页打包、CRC、Flash WIP 等待、命令解析或 UART 打印。这样做有三个目的：降低中断
抖动、避免低优先级工作阻塞高优先级采样、让复杂错误处理只在单一前台上下文发生。

## 6. 定时器触发 ADC：为什么采样频率不依赖 CPU

### 6.1 时钟推导

系统时钟 168 MHz，APB1 分频为 4，因此 PCLK1=42 MHz。STM32F4 在 APB prescaler 不为 1
时，Timer clock 为 2×PCLK，所以 TIM2 输入时钟是 84 MHz。

设计先把 TIM2 降到 1 MHz：

```text
PSC = 84 MHz / 1 MHz - 1 = 83
ARR = 1 MHz / 10 kHz - 1 = 99
```

每 100 μs 产生 update event，即 10,000 sample/s。`TIM2->CR2.MMS=010` 将 update 选为
TRGO，ADC1 外部触发选择 TIM2_TRGO。CPU 即使正在做 CRC，也不会改变触发间隔。

### 6.2 核心寄存器配置

摘自 `Platform/Src/tracebox_adc_dma.c` 的等价简化：

```c
RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

TIM2->CR2 = TIM_CR2_MMS_1;   /* update -> TRGO */
TIM2->PSC = 83U;
TIM2->ARR = 99U;
TIM2->EGR = TIM_EGR_UG;

ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0; /* /4 */
ADC1->SMPR2 = 3UL << ADC_SMPR2_SMP0_Pos;                    /* 56 cycles */
ADC1->SQR3 = 0U;                                             /* channel 0 */
ADC1->CR2 = ADC_CR2_DMA | ADC_CR2_DDS |
            (6UL << ADC_CR2_EXTSEL_Pos) | ADC_CR2_EXTEN_0 |
            ADC_CR2_ADON;
```

ADC clock 是 84 MHz / 4 = 21 MHz，低于 F407 的 36 MHz 上限。PA0 配成 analog 且关闭上下拉。

## 7. ADC DMA Double Buffer 和完成描述符环

### 7.1 为什么每块是 96 个样本

ADC 数据按 `uint16_t` 保存：96 × 2 = 192 B。它正好能与 32 B 页头组成 224 B CRC 覆盖区，
再加 31 B reserved 和 1 B commit，形成一个 256 B W25Q 物理页。

```text
96 samples / 10,000 samples/s = 9.6 ms/block
```

因此 DMA 每 9.6 ms 完成一次 buffer，Recorder 也每 9.6 ms 产生一个 Flash page。

### 7.2 DBM 配置

```c
DMA2_Stream4->PAR  = (uint32_t)&ADC1->DR;
DMA2_Stream4->M0AR = (uint32_t)s_adc.buffers[0];
DMA2_Stream4->M1AR = (uint32_t)s_adc.buffers[1];
DMA2_Stream4->NDTR = 96U;
DMA2_Stream4->CR = DMA_SxCR_PL_1 |
                   DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 |
                   DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_DBM |
                   DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
```

- `PSIZE/MSIZE=01`：16 bit；
- `MINC=1`：内存地址递增；
- `CIRC=1`：循环；
- `DBM=1`：M0/M1 双缓冲；
- `TCIE/TEIE/DMEIE`：完成和错误中断。

启动顺序是：停 Timer → 清 DMA flag → 配地址和 NDTR → 开 DMA Stream → 开 ADC DMA/触发 →
最后开 Timer。这样 Timer 不会在 DMA 准备好之前产生第一个转换。

### 7.3 ISR 如何判断刚完成的是哪块

DMA `CT` 表示“当前正在使用的目标”。完成切换发生后，刚完成的是另一个目标：

```c
descriptor->buffer_index =
    ((DMA2_Stream4->CR & DMA_SxCR_CT) != 0U) ? 0U : 1U;
```

ISR 发布的不是一个布尔 flag，而是：

```c
typedef struct {
    uint32_t sequence;
    uint32_t completed_at_cycles;
    uint16_t sample_count;
    uint8_t  buffer_index;
    uint8_t  reserved;
} TbAdcDmaDescriptor;
```

如果只设 `adc_done=true`，主循环被延迟两个 DMA 周期时，两次完成会合并成一次，且无法知道
丢了哪块。单调 `published/consumed` 计数与四项描述符环能发现事件积压和序号缺口。

### 7.4 ISR 发布顺序和内存屏障

伪代码：

```c
on_dma_tc_irq:
    clear_dma_flags()
    completed = opposite_of(DMA_CT)
    depth = published - consumed

    if depth < DESCRIPTOR_CAPACITY:
        ring[published & 3] = { seq, dwt_now, 96, completed }
        DMB()                    // 先让描述符内容对前台可见
        published++             // 再发布
        update_high_water()
    else:
        descriptor_overflow++
```

前台反向执行：先读 `published`，`DMB()` 后复制对应描述符，再增加 `consumed`。这里没有动态
内存，也没有在 ISR 中复制 192 字节采样。

### 7.5 双缓冲的重要时间约束

描述符环有 4 项，但物理采样区只有 2 块。描述符只能缓冲“完成通知”，不能延长采样数据的
生命周期。主循环必须在 DMA 再次回到同一 M0/M1 之前完成 192 B 快照，理论窗口约 9.6 ms。
正式 60 秒测试中页打包最大 31.0 μs，远低于窗口。

## 8. 固定页池：生产者与消费者怎样解耦

### 8.1 内存布局

页池静态分配 8 个 256 B slot，总计 2 KiB。实际使用 `uint32_t pages[8][64]` 保证自然对齐，
对外转换成 `uint8_t *`。

READY queue 只存一个字节的 slot index，不再复制 256 B 数据：

```text
slot data 由状态决定唯一 owner

FREE -> FILLING -> READY -> WRITING -> FREE
         packer     queue     NOR writer
```

### 8.2 为什么需要状态而不只是 head/tail

仅有 ring head/tail 能说明队列顺序，不能证明某块内存当前是否允许写。slot state 可以检测：

- 重复 publish；
- READY slot 被提前 release；
- 非法 index；
- writer 完成回调重复释放；
- acquire 后异常退出时未释放。

关键接口：

```c
TbStatus TbPagePool_Acquire(uint8_t *index, uint8_t **page);
TbStatus TbPagePool_Publish(uint8_t index);
TbStatus TbPagePool_TakeReady(uint8_t *index, const uint8_t **page);
TbStatus TbPagePool_Release(uint8_t index);
```

每个接口先检查空指针、index 范围和当前状态。无空闲 slot 时返回 `TB_STATUS_OVERFLOW`，不
覆盖还未持久化的旧数据。非法所有权迁移单独计入 `ownership_error_count`。

### 8.3 为什么 8 页不是为了掩盖吞吐不足

8 页最多吸收 8 × 9.6 = 76.8 ms 的短时 Flash 抖动。如果 Flash 平均写入速度慢于生产速度，
池最终必满。正确策略是记录 HWM/overflow 并安全停止，而不是继续扩大 RAM 假装系统稳定。

60 秒硬件结果为：page pool HWM=1/8，overflow=0。这说明在当前 SCK 和 W25Q64 上，消费者
长期速率高于生产者，而不是依赖大池勉强撑满 60 秒。

## 9. 页格式：协议不能依赖 C 结构体内存布局

### 9.1 256 字节页

| Offset | Size | 字段 | 内容 |
|---:|---:|---|---|
| 0 | 4 | magic | `TBP1` |
| 4 | 1 | version | 1 |
| 5 | 1 | record_type | 1=ADC |
| 6 | 1 | header_size | 32 |
| 7 | 1 | flags | 页标志 |
| 8 | 4 | page_sequence | 捕获内从 0 单调递增 |
| 12 | 4 | t0_us | 本页第一个样本时间 |
| 16 | 4 | sample_period_ns | 100000 |
| 20 | 2 | sample_count | 96 |
| 22 | 2 | payload_length | 192 |
| 24 | 4 | page_crc32 | CRC 字段计算时按 0 处理 |
| 28 | 4 | capture_id | 一次采集的身份 |
| 32 | 192 | samples | 96×uint16，小端 |
| 224 | 31 | reserved | 0xFF |
| 255 | 1 | commit | 0xFF=未提交，0x00=有效提交 |

### 9.2 显式序列化

不能执行：

```c
flash_write((uint8_t *)&some_c_struct, sizeof(some_c_struct));
```

原因包括 padding、alignment、编译器 ABI 和大小端。项目通过 `Page_WriteU16Le` 和
`Page_WriteU32Le` 明确每一个字节：

```c
static void Page_WriteU32Le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
    dst[2] = (uint8_t)(value >> 16U);
    dst[3] = (uint8_t)(value >> 24U);
}
```

这使 Python `struct.unpack_from("<IIIHH", ...)` 能独立解析同一物理字节。

### 9.3 时间戳为什么不用 60 秒 DWT 差值

DWT CYCCNT 是 32 bit，在 168 MHz 下约 25.6 s 回卷。因此它适合测 ISR、页打包和一次 SPI
事务，不适合直接表示完整 60 秒捕获时长。

V1 页时间轴使用：

```c
t0_us = page_sequence * 9600U;
sample_time = t0_us + sample_index * 100U;
```

整体 `capture_ms` 使用 32 bit HAL tick 的无符号差值。这样 60 秒不会受 DWT 回卷影响。

## 10. 硬件 CRC 与 MCU/PC 一致性

V1 使用 CRC-32/MPEG-2：poly `0x04C11DB7`、init `0xFFFFFFFF`、不反射、xorout 0。
覆盖 offset 0..223；计算时 offset 24..27 的 CRC 字段视为 0；不覆盖 reserved 和 commit。

F407 硬件 CRC 单元按 32 bit word 接收。为了避免 CPU 字节序造成歧义，页编码器显式将四个
字节组装成大端数值再写 `CRC->DR`：

```c
word = ((uint32_t)page[n]     << 24U) |
       ((uint32_t)page[n + 1] << 16U) |
       ((uint32_t)page[n + 2] <<  8U) |
        (uint32_t)page[n + 3];
CRC->DR = word;
```

Python 端用逐字节 CRC 实现，不调用 MCU 代码。如果两端对确定性页得到相同 CRC，说明页布局、
覆盖范围、字节序和算法参数全部一致。项目公共向量包括：

- `"123456789" -> 0x0376E6E7`；
- 固定 ADC 页 -> `0x9251236D`；
- NOR 自测页 -> `0x56E7AE59`。

PC 再翻转 payload 一位，必须得到 CRC mismatch。这证明校验器不是只比较预先写死的 PASS。

## 11. SPI1 寄存器级驱动

### 11.1 SPI 静态配置

SPI1 运行在 Mode 0 默认配置，APB2 peripheral clock=84 MHz。初始最大 SCK 要求为
1.3125 MHz，驱动从 `{2,4,8,16,32,64,128,256}` 中选择第一个不超过目标的分频，因此：

```text
84 MHz / 64 = 1.3125 MHz
```

GPIOA PA5..PA7 设为 AF5、push-pull、高速、无上下拉；PB0 是软件 CS。CS 操作使用 BSRR：

```c
static void Spi_SetChipSelect(bool selected)
{
    if (selected) {
        GPIOB->BSRR = 1UL << (0U + 16U); /* atomic reset -> CS low */
    } else {
        GPIOB->BSRR = 1UL << 0U;         /* atomic set -> CS high */
    }
}
```

与 `ODR &= ~mask` 相比，BSRR 是单次原子写，不会出现 read-modify-write 干扰其他 GPIO bit。

### 11.2 Polling 基线

Polling 传输每个字节执行：等 TXE → 写 DR → 等 RXNE → 读 DR；全部字节完成后还要等 BSY=0
才能拉高 CS。每个等待都共享一个 DWT deadline：

```c
for each byte:
    wait TXE or timeout/error
    write SPI1->DR
    wait RXNE or timeout/error
    read SPI1->DR
wait BSY == 0 or timeout/error
deassert CS
```

它的价值不是最终性能，而是提供最简单、可观察的寄存器/连线基线。PA7→PA6 loopback 下，
PRBS 发送值必须逐字节原样返回。

### 11.3 DMA transaction descriptor

```c
typedef void (*TbSpiCompletionCallback)(TbStatus status,
                                        void *user_context);

typedef struct {
    const uint8_t *tx_data;
    uint8_t *rx_data;
    uint32_t length;
    uint32_t timeout_cycles;
    TbSpiCompletionCallback completion_callback;
    void *user_context;
} TbSpiDmaTransfer;
```

这里体现了几项重要 C 语言能力：

- `const uint8_t *` 表示驱动只读发送区；
- RX 是可写指针；
- callback 将驱动与上层行为解耦；
- `void *context` 让同一回调可关联不同事务；
- descriptor 只复制元数据，不复制大 payload，因此调用者必须保证 buffer 在 callback 前有效；
- API 在写寄存器前检查 null、长度 1..4096、deadline 和当前 state。

### 11.4 SPI DMA 配置和启动顺序

RX：DMA2 Stream2 Channel3，peripheral→memory；TX：Stream3 Channel3，memory→peripheral。
两者都是 byte、normal、memory increment。

简化代码：

```c
DMA2_Stream2->PAR  = (uint32_t)&SPI1->DR;
DMA2_Stream2->M0AR = (uint32_t)rx;
DMA2_Stream2->NDTR = length;
DMA2_Stream2->CR   = CH3 | MINC | TCIE | TEIE | DMEIE;

DMA2_Stream3->PAR  = (uint32_t)&SPI1->DR;
DMA2_Stream3->M0AR = (uint32_t)tx;
DMA2_Stream3->NDTR = length;
DMA2_Stream3->CR   = CH3 | MINC | M2P | TCIE | TEIE | DMEIE;

CS_LOW();
started_at = DWT->CYCCNT;
state = DMA_ACTIVE;
DMB();
SPI1->CR2 |= RXDMAEN | TXDMAEN;
DMA2_Stream2->CR |= EN;   /* RX first */
DMA2_Stream3->CR |= EN;   /* TX then creates clocks */
return TB_STATUS_OK;
```

必须先关 Stream 并等待硬件把 EN 清零，才能重新配置 `PAR/M0AR/NDTR/CR`；必须先开 RX 再开
TX，防止首字节到来时 RX 尚未就绪。

### 11.5 为什么两个 DMA TC 都到还不能立刻拉高 CS

DMA TC 表示内存与 SPI DR 的搬运已经结束，不严格等于移位寄存器最后一位已经出线。因此前台
收到 RX_DONE+TX_DONE 后仍检查 `SPI1->SR.BSY==0`，再关闭 DMA request、禁用 Stream、拉高
CS 并调用 callback。

SPI ISR 只做：读 DMA flag、清 flag、累加 IRQ/错误计数、置 RX_DONE/TX_DONE/ERROR event。
`TbSpi_MainFunction()` 才完成 BSY、deadline、cleanup 和 callback。

### 11.6 状态机

```text
RESET --init--> READY
READY --polling--> POLLING --complete--> READY
READY --dma start--> DMA_ACTIVE
DMA_ACTIVE --RX+TX done and BSY=0--> READY + callback(OK)
DMA_ACTIVE --TE/DME/SPI error/timeout--> recover --> READY + callback(error)
```

如果 state 不是 READY，新事务返回 `TB_STATUS_BUSY`。这样上层不能在前一事务仍持有 CS 和
DMA Stream 时重写寄存器。

### 11.7 超时后的恢复为什么要复位 SPI1

第一次 v0.3.1 故障注入中，短 deadline 正确产生 TIMEOUT，但下一笔正常 4 KiB loopback 在
byte 0 出现不匹配。原因被定位为中途 abort 后 SPI DR/shift 状态残留。只清 SPE 不够。

修正后的恢复顺序：

1. 关闭 SPI 的 RXDMAEN/TXDMAEN；
2. 关闭两个 Stream，并等待 EN 清零；
3. CS 拉高；
4. 通过 `RCC->APB2RSTR.SPI1RST` 复位 SPI1；
5. 恢复保存的 CR1、CR2、CRCPR；
6. 清 DR/SR/CRCERR、DMA flag 和 NVIC pending；
7. 重新使能 SPI，state 回 READY；
8. 在前台发布原始 TIMEOUT callback。

硬件复测中，强制超时后不复位 MCU，立即启动下一笔 4 KiB PRBS DMA，结果完全匹配。这是一个
很好的“失败证据 → 根因定位 → 恢复策略升级 → 回归通过”面试案例。

## 12. W25Q64 异步 NOR 状态机

### 12.1 为什么 SPI driver 和 Flash driver 必须分开

SPI driver 只理解“在一次 CS 窗口内交换 N 个字节”；W25Q driver 才理解：

- `0x9F` JEDEC ID；
- `0x06` Write Enable；
- `0x05` Read Status Register 1；
- `0x02` Page Program；
- `0x20` 4 KiB Sector Erase；
- `0x03` Read Data；
- WEL/WIP bit、24-bit address、页边界和器件 deadline。

这样 SPI loopback 失败时不用怀疑 W25Q 命令，W25Q WEL 失败时也不用修改通用总线驱动。

### 12.2 地址与边界检查

公开 API 在启动事务前检查：

- 指针不为空，长度不为 0；
- read/program 单次长度不超过 256；
- `page_offset + length <= 256`，拒绝 page crossing；
- sector erase 地址 4096 对齐；
- 地址和长度不发生 24-bit 溢出；
- JEDEC 识别后不超过真实容量；
- state 必须 READY；
- timeout 不得为 0。

特别是表达式 `length <= limit - address` 比 `address + length <= limit` 更不容易发生加法溢出。

### 12.3 Program 为什么先复制调用者数据

`TbNor_PageProgramStart()` 把最多 256 B 输入复制到 NOR context 的 `program_data[]`。因为
这是异步 API，调用者返回后可能立即复用原 buffer；若 NOR 保存原指针，后续 DMA 看到的内容
可能已经变化。复制增加少量成本，却建立了明确的 buffer lifetime。

Recorder 的 page slot 仍一直保持 WRITING 直到 body+commit 完成，这是第二层所有权保护。

### 12.4 Erase/Program 状态流

```text
WRITE_ENABLE_LAUNCH
 -> WRITE_ENABLE_WAIT
 -> WEL_POLL_LAUNCH
 -> WEL_POLL_WAIT
 -> COMMAND_LAUNCH
 -> COMMAND_WAIT
 -> BUSY_POLL_LAUNCH
 -> BUSY_POLL_WAIT
      | WIP=1: next_poll=now+1ms，返回主循环
      | WIP=0: 更新统计并 callback(OK)
      | deadline: callback(TIMEOUT)，state=FAULT
```

`TbNor_MainFunction(now_ms)` 每次只推进一个短阶段，从不 `while(WIP)`。WIP 每 1 ms 读一次，
既避免忙等，也不会用密集 SPI status query 占满总线。

关键伪代码：

```c
case BUSY_POLL_WAIT:
    if (spi_completed_ok()) {
        sr1 = rx[1];
        if ((sr1 & WIP) == 0) {
            finish(OK);
        } else {
            next_poll_ms = now_ms + 1;
            phase = BUSY_POLL_LAUNCH;
        }
    }
    break;
```

### 12.5 NOR timeout 后怎样恢复

操作级 deadline 到期后，软件 state 进入 FAULT，但物理芯片可能仍在内部 erase/program。此时
不能直接发送下一条修改命令。`TbNor_RecoverStart()` 从 FAULT 或 READY 进入受 deadline 约束
的 WIP 轮询，直到芯片真正 idle，再把 state 恢复为 READY。

实机 `nor selftest` 用 1 ms erase deadline 故意制造 timeout，随后成功 recover，证明系统不会
无限等待，也不会在芯片仍 busy 时盲目继续。

## 13. Recorder：把三个独立模块组成闭环

### 13.1 顶层状态

```text
IDLE
  -> PREPARING -> PREPARED
  -> RECOVERING -> RECOVERED

PREPARED -> RECORDING -> DRAINING -> VERIFYING -> COMPLETE

任一关键错误 -> ERROR
```

`operation` 与 `state` 分开：state 表示当前生命周期位置，operation 表示本次用户意图
（PREPARE/RECORD/RECOVER/SELFTEST）。`last_status` 保存分类结果。

### 13.2 为什么 `prepare` 和 `record` 分开

Sector erase 最坏时间明显大于一次 page program。如果记录期间才擦下一 sector，会与每 9.6
ms 到来的生产者竞争。V1 先根据目标页数擦完固定窗口：

```c
sectors_required = (target_pages + 15U) / 16U;
```

一个 4 KiB sector 包含 16 个 256 B page。60 秒需要 6,250 page，即擦 391 sector，得到 6,256
page capacity。只有 PREPARED 才能进入 RECORDING；record 秒数不能超过已准备容量。

### 13.3 Producer 主循环

`Recorder_ServiceProducer()` 的核心步骤：

```c
while (state == RECORDING && adc_descriptor_available()) {
    descriptor = pop_descriptor();
    check_descriptor_sequence();

    samples = adc_buffer(descriptor.buffer_index);
    slot, page = page_pool_acquire();

    metadata = {
        .page_sequence = pages_packed,
        .t0_us = pages_packed * 9600,
        .sample_period_ns = 100000,
        .capture_id = capture_id,
    };

    started = DWT_CYCCNT;
    build_adc_page(page, metadata, samples);  // 192 B snapshot + CRC
    mark_commit_in_ram(page);                 // RAM target byte becomes 0x00
    update_pack_cycle_max(DWT_CYCCNT - started);

    page_pool_publish(slot);
    pages_packed++;

    if (pages_packed == target_pages) {
        stop_adc_dma();
        state = DRAINING;
    }
}
```

这里先在 RAM 把 offset 255 置为 0，是为了让 writer 能从同一页拿到最终 commit 值；真正写
Flash 时仍分为 0..254 和 255 两次 program。

任何 acquire/build/publish 失败都会停止本次记录并保留分类错误，而不是静默丢块继续。

### 13.4 Consumer 主循环

Writer 同时最多有一页 active：

```c
if ((state == RECORDING || state == DRAINING) && !writer_active) {
    if (page_pool_take_ready(&slot, &page) == OK) {
        writer_active = true;
        nor_program(address, page, 255, body_done_callback);
    }
}

body_done_callback:
    nor_program(address + 255, &page[255], 1, commit_done_callback)

commit_done_callback:
    page_pool_release(slot)
    pages_committed++
    writer_active = false
```

Recorder 本身不访问 SPI/DMA 寄存器，也不拼 WREN/WIP 命令。它只组合下层接口和管理 slot
所有权。

### 13.5 DRAINING 的意义

当目标 ADC block 已经产生，Timer/ADC DMA 停止，但页池可能还有 READY page，NOR 也可能正在
写最后一页。如果直接标记 COMPLETE，会丢掉已采集但尚未持久化的数据。

DRAINING 等待以下条件同时成立：

- `writer_active == false`；
- `pages_committed == target_pages`；
- `ready_depth == 0`。

然后才进入全量 VERIFYING，逐页读回检查格式、commit、CRC、sequence 和 capture_id。

## 14. 两阶段提交与掉电恢复

### 14.1 NOR 的单向写入约束

擦除将 bit 置为 1，program 只能把 1 改成 0。commit 初始是 `0xFF`，最后写 `0x00`，因此无需
擦除即可完成一次原子语义的“有效性发布”。

写一页的逻辑顺序：

```text
ERASED PAGE
  body=FF..., commit=FF

PROGRAM BODY 0..254
  header/payload/crc 已存在, commit 仍 FF  -> 不可见

PROGRAM COMMIT 255 = 00
  body + commit 完整                     -> 可见
```

### 14.2 为什么 CRC 不能替代 commit

如果断电恰好发生在 program 尾部，部分页可能碰巧保留一个看似合理的 header，甚至极低概率
CRC 也可能碰撞。commit 明确表达“软件已经确认 body WIP 清零，并开始完成第二阶段”。恢复时
要求 magic/version/length/commit/CRC/sequence/capture_id 全部成立。

CRC 负责“内容有没有损坏”，commit 负责“写入协议有没有走到完成点”，两者语义不同。

### 14.3 启动恢复算法

每次 boot 都从 `0x002000` 扫描，最多 6,250 页：

```c
valid_pages = 0
expected_sequence = 0
capture_id = UNKNOWN

for page_index in [0, scan_limit):
    page = nor_read(data_base + page_index * 256)
    scanned++

    if page is all 0xFF:
        stop successfully                // clean end

    if invalid format/commit/CRC:
        rejected_torn++
        stop successfully                // invalid tail is not history

    if sequence != expected_sequence:
        sequence_errors++
        rejected_torn++
        stop successfully

    if page.capture_id != first_capture_id:
        sequence_errors++
        rejected_torn++
        stop successfully

    valid_pages++
    expected_sequence++
```

为什么遇到第一个坏页就停止，而不是跳过继续扫描？因为 V1 是 append-only 线性窗口，有效历史
必须是连续前缀。跳过坏页会把旧 session 残留或随机字节误认为新历史。

恢复只读、不擦除、不在 torn page 后原地续写。继续记录必须重新 `prepare`，这是 V1 用简单
规则换取可证明一致性的范围选择。

### 14.4 已完成的 torn-page 证据

`fault torn` 在第 312 页之后构造完整 body 和 CRC，但只 program 0..254，故意不写 commit。
物理按 RST 后，boot recovery 得到：

```text
valid_pages=312
scanned_pages=313
rejected_torn_pages=1
sequence_errors=0
crc_errors=0
```

这证明恢复保留了全部已提交前缀，并没有把结构完整但 commit 缺失的页算作有效。三次不可控
物理拔电仍属于待补可靠性证据，不应与此测试混称。

## 15. 分类错误码、deadline 和异常策略

统一错误枚举：

```c
typedef enum {
    TB_STATUS_OK               =  0,
    TB_STATUS_INVALID_ARGUMENT = -1,
    TB_STATUS_TIMEOUT          = -2,
    TB_STATUS_OVERFLOW         = -3,
    TB_STATUS_HW_ERROR         = -4,
    TB_STATUS_BUSY             = -5,
    TB_STATUS_VERIFY_FAILED    = -6,
    TB_STATUS_CRC_ERROR        = -7,
    TB_STATUS_STATE_ERROR      = -8,
    TB_STATUS_RANGE_ERROR      = -9,
    TB_STATUS_FORMAT_ERROR     = -10
} TbStatus;
```

错误码回答的是不同问题：

- `INVALID_ARGUMENT`：调用契约错误，例如 null/zero timeout；
- `RANGE_ERROR`：数值本身超出允许范围；
- `BUSY`：资源暂时被合法事务占用；
- `STATE_ERROR`：生命周期不允许，例如未 prepare 就 record；
- `TIMEOUT`：硬件或器件未在 deadline 内完成；
- `HW_ERROR`：DMA TE/DME、SPI MODF/OVR 等；
- `CRC_ERROR/FORMAT_ERROR/VERIFY_FAILED`：持久化内容或器件身份不可信；
- `OVERFLOW`：固定队列无法接纳新数据。

deadline 使用无符号差值或有符号时间比较以处理计数器回卷：

```c
expired_cycles = (uint32_t)(now_cycles - started_cycles) >= timeout_cycles;
time_reached   = (int32_t)(now_ms - deadline_ms) >= 0;
```

异常总体策略是“首错可见、安全停止、释放硬件、显式恢复”，而不是无限自动重试。复杂重试会
掩盖首个故障，也难以在一周内证明所有组合。

## 16. 可观测性：每个“没问题”都要对应一个计数器

### 16.1 ADC 指标

- `blocks_produced/consumed`：硬件产生和前台处理的块数；
- `dma_irq_count`：DMA 完成/错误 IRQ 数；
- `dma_error_count`、`adc_overrun_count`；
- `descriptor_pending/high_water/overflow_count`；
- `last_isr_cycles/max_isr_cycles`。

### 16.2 Page pool 指标

- acquire/publish/take/release 总数；
- `in_use` 和 `in_use_high_water`；
- `ready_depth` 和 `ready_high_water`；
- `overflow_count` 与 `ownership_error_count`。

### 16.3 SPI/NOR 指标

- Polling/DMA transaction 与 byte 总数；
- DMA IRQ/error、SPI hardware error、timeout/recovery；
- `last/max transfer cycles`；
- identify/erase/program/read 次数和字节数；
- status poll、WEL error、NOR timeout/recovery。

### 16.4 Recorder 指标

- target/prepared sector/page；
- ADC block、packed/committed/verified/valid page；
- recovery scanned/rejected torn；
- sequence/CRC/Flash error；
- pack cycles、capture time；
- 从 ADC/PagePool 汇总进来的 HWM/overflow/error。

读取由 ISR 修改的统计快照时，`TbSpi_GetStats()` 和 `TbAdcDma_GetStats()` 保存 PRIMASK、短暂关
中断、复制结构体，再恢复原中断状态。临界区只包围固定长度内存复制，不做 I/O 或等待。

## 17. 性能结果与正确解释

### 17.1 4 KiB Polling / DMA 基准

同一 PA7→PA6 loopback、同一 1.3125 MHz SCK、同一 4096 B PRBS：

| 模式 | transfer cycles | 吞吐 | 启动/阻塞 cycles | 前台循环次数 |
|---|---:|---:|---:|---:|
| Polling | 4,595,250 | 149,747 B/s | 4,595,250 | 0 |
| DMA | 4,194,367 | 164,060 B/s | 302 | 23,881 |

理论字节线速率：

```text
1,312,500 bit/s / 8 = 164,062.5 byte/s
```

DMA 实测 164,060 B/s，接近物理线速率。Polling 较慢是因为软件逐字节检查 flag 和访问 DR。

### 17.2 `302 cycles` 应该怎样说

302 cycles 是 DMA 启动 API 从进入到返回的成本；Polling 的 4,595,250 cycles 是同步函数直到
传输结束的阻塞时间。因此可以说：

> DMA API 约 302 cycles 即返回，随后硬件搬运期间合作式前台继续运行 23,881 次。

不能说：

> 整机 CPU 占用下降了 99.99%。

因为当前没有将整个 DMA 期间的 MainFunction/ISR/callback cycles 全部积分为统一 CPU active
时间。精确 CPU utilization 和 idle ratio 是 P1 指标。

### 17.3 ADC 专项结果

200 块/19,200 样本专项实测：

- 199 个块间隔平均 1,612,799 cycles；预期 1,612,800 cycles；
- min/max 1,612,664 / 1,612,934 cycles；
- descriptor HWM 1/4；
- DMA/ADC/sequence/buffer error 0；
- ADC ISR max 117 cycles。

注意这一专项使用 9.6 ms × 168 MHz = 1,612,800 cycles。正式页时间轴使用 9,600 μs。

### 17.4 正式 60 秒结果

| 指标 | MEASURED |
|---|---:|
| 采样率/时间 | 10 kS/s / 60,000 ms |
| 样本数 | 600,000 |
| packed/committed/verified pages | 6,250 / 6,250 / 6,250 |
| dump | 1,600,000 B |
| PC 重建时间 | 59.9999 s |
| sequence/CRC/Flash errors | 0 / 0 / 0 |
| page pool/descriptor overflow | 0 / 0 |
| ADC DMA errors/overruns | 0 / 0 |
| page pool/descriptor HWM | 1 / 1 |
| pack max | 5,216 cycles ≈ 31.0 μs |

31.0 μs 相对 9.6 ms block 周期约为 0.323%，说明 192 B 快照、页头编码和硬件 CRC 不构成当前
生产周期瓶颈。这个百分比只描述最坏 pack step 占 block period，不是整机 CPU 占用。

## 18. UART 命令和二进制 dump

### 18.1 为什么控制和数据使用不同波特率

控制命令短且需要兼容，固定 115200 8N1；1.6 MB raw dump 若继续低速会拖长演示，因此支持
230400/460800。流程是：

```text
115200: MCU -> "TB_DUMP_BEGIN <pages> 256 <baud>\r\n"
双方切到 announced baud
MCU -> 精确 pages * 256 个 raw bytes
MCU -> "\r\nTB_DUMP_END\r\n"
双方恢复 115200
MCU -> DUMP RESULT
```

raw payload 可以包含任意 `0x00/0x0A/0xFF`，因此不能靠换行判断结束。PC 必须从 preamble 计算
精确长度，少一个字节都报 incomplete dump。

### 18.2 Dump 也是状态机

App 中的阶段是：

```text
IDLE -> ANNOUNCE -> SWITCH_DELAY -> READ_START -> READ_WAIT
     -> SEND_PAGE -> ... -> END -> RESTORE_DELAY -> RESULT -> IDLE
                                  \-> ERROR -> IDLE
```

Flash 每次异步读一页，callback 只切换到 SEND_PAGE；前台再通过 UART 发 256 B。出错时检查当前
baud 并尽量恢复 115200，避免设备之后无法控制。

USART TX 当前是带 spin 上限的寄存器 polling，因为它是导出阶段的非实时链路；USART RX 使用
128 B 中断环。记录期间只允许输出很短的 progress 行，因为长 metrics 文本曾占用超过一个
9.6 ms 采样块周期。这个修改体现了“可观测性本身不能破坏被观测系统”。

## 19. Python 工具：独立证据链

### 19.1 为什么 Python 是唯一串口和 codec 实现

`tools/tracebox_cli.py` 负责：

- 打开唯一 COM owner；
- `prepare/record/recover/dump/full/fault-torn`；
- 等待完整终态行并归档 raw log；
- 根据 preamble 精确接收二进制；
- 保存 `.tbdump`；
- 调用统一 page codec 和 exporter。

`tools/record_codec.py` 独立实现页布局和 CRC；`tools/dump_validate.py` 逐页检查并导出 CSV、
JSON、Markdown。

Node server 不重复串口，浏览器也不重复 CRC。这样 CLI 和 localhost 对同一 dump 一定得到同一
判断。

### 19.2 PC 解码核心

```python
def decode_adc_page(page: bytes) -> AdcPage:
    if len(page) != 256:
        raise RecordFormatError("bad size")
    if page[0:4] != b"TBP1":
        raise RecordFormatError("bad magic")
    if page[255] != 0x00:
        raise RecordFormatError("not committed")

    stored_crc = struct.unpack_from("<I", page, 24)[0]
    if stored_crc != page_crc_with_crc_field_zero(page):
        raise RecordFormatError("CRC mismatch")

    return decode_explicit_little_endian_fields(page)
```

Exporter 还检查：page sequence 是否等于物理 page index、capture_id 是否一致，并计算 ADC 的
min/max/average 和重建时长。

### 19.3 一个真实的主机端 bug

最初状态解析器看到串口分片 `status=O` 就立即判断不是 `OK`，误报设备拒绝。实际上串口 byte
stream 可以在任何位置分段。修复后正则只有在 CR/LF 完整行到达后才解释状态，并增加回归测试。

这个案例说明：UART 是字节流，不提供应用消息边界；上位机协议同样需要 framing。

## 20. Localhost 中文控制台

### 20.1 架构

```text
Browser
  -> HTTP JSON
Node built-in HTTP server（single-operation coordinator）
  -> spawn 参数数组，shell=false
Python tracebox_cli.py
  -> pyserial
STM32 TraceBox
```

server 只绑定 `127.0.0.1:8765`。UI 提供中文连接、采集、恢复、导出、指标和文件下载，但不
允许用户输入任意 shell/固件命令。

### 20.2 为什么需要 single-operation coordinator

串口只能被一个进程占用。`OperationCoordinator` 在有 queued/running job 时对新任务返回 HTTP
409；子进程 close/error 时将 job 置为 succeeded/failed，避免页面永久卡在 running。

允许动作是白名单：

```text
info, metrics, prepare, record, recover, dump,
fault-torn, full, bench
```

端口只接受 `COM1..COM999`，seconds 限定 1..60，dump baud 限定三种值。子进程通过参数数组
启动且 `shell:false`，浏览器文本不能进入命令字符串。

### 20.3 文件下载边界

只枚举 `artifacts/raw` 和 `artifacts/exports` 中允许后缀的普通文件。请求名必须等于
`path.basename(name)`，因此 `../` 路径穿越会被拒绝。

当前 localhost 的中文 UI、5 项 Node/API 测试和 COM9 实机 HTTP 链路已经完成。`info` 与
`3s full` 均由页面使用的 operation API 启动，artifact route 下载的 79,872 B dump 与本地
原文件 SHA-256 一致。演示前仍可补一次鼠标点击视觉冒烟，但不需要重烧固件。

## 21. 从上电到生成 CSV 的完整时序

下面是一次 `full --seconds 60` 的全链路：

```text
PC CLI                 App/Recorder              NOR/SPI              ADC/DMA
  |                         |                       |                     |
  |-- prepare 60 ---------->|                       |                     |
  |                         |-- JEDEC ID ---------->|                     |
  |                         |-- erase 391 sectors ->| WREN/WIP polls      |
  |<-- PREPARE RESULT ------|                       |                     |
  |                         |                       |                     |
  |-- record 60 ----------->|-- start ADC -------------------------------->|
  |                         |<-- desc every 9.6ms --------------------------|
  |                         |-- pack page -> pool    |                     |
  |                         |-- program body ------->| SPI DMA             |
  |                         |-- program commit ----->| SPI DMA             |
  |                         |   ... x6250 ...        |                     |
  |                         |-- stop + drain         |                     |
  |                         |-- read/verify x6250 -->|                     |
  |<-- RECORD RESULT -------|                       |                     |
  |                         |                       |                     |
  |-- dump 460800 --------->|-- announce length     |                     |
  |<== 1,600,000 raw bytes =|<-- read pages --------|                     |
  |                         |                       |                     |
  | PC codec: commit + CRC + sequence + capture_id                       |
  | -> .tbdump + .csv + .json + .md + hashes                             |
```

## 22. 关键不变量

读源码时优先记住这些不变量，而不是死记函数：

1. 同一时刻一个 page slot 只有一个 owner；
2. ADC completed buffer 必须在 DMA 再次使用它之前被快照；
3. SPI DMA active 时 TX/RX buffer 不得失效或修改；
4. DMA Stream 配置前必须 EN=0，flag 必须清除；
5. DMA TC 后仍要等 SPI BSY=0 才能释放 CS；
6. NOR program/erase 前必须 WREN，并验证 WEL；
7. WIP 等待必须有 deadline，禁止无限循环；
8. body 成功不等于 page 提交，commit 必须单独成功；
9. recovery 只接受从 data base 开始的连续有效前缀；
10. 任何 TARGET 数字在产生实测证据前不得写成 MEASURED。

## 23. V2：接入 Mini-DCM 的三板运行时黑匣子

### 23.1 V2 的价值边界

V2 不需要更新 V1 的可靠存储核心。它增加的是一个外部结构化事件 producer，让 TraceBox 从
“自己采 ADC 的记录器”升级成“能观察另一个嵌入式系统的独立黑匣子”。

三板可以理解为：

```text
F103 ECU <---- CAN/UDS ----> F407 Tester
                               |
                               | SPI master + TraceFrame
                               v
                         F407 TraceBox
                         SPI slave RX DMA
                               |
                               v
                         W25Q64 + PC timeline
```

Mini-DCM 的 CAN/UDS 核心无需搬进 TraceBox；它只在关键状态变化处调用轻量 `TraceHook_Emit()`。
这样 TraceBox 与被观察系统分离，即使 Mini-DCM 复位，已提交历史仍在独立 Flash 中。

### 23.2 推荐硬件资源

保留 TraceBox SPI1 作为 W25Q64 master。外部输入优先使用 SPI2 slave，使输入与存储使用不同
外设和 DMA 控制器候选：

| V2 功能 | 候选资源 | 说明 |
|---|---|---|
| SPI2 NSS/SCK/MISO/MOSI | PB12/PB13/PB14/PB15 AF5 | 开工前必须再核对核心板 FPC/原理图冲突 |
| SPI2_RX DMA | DMA1 Stream3 Channel0 候选 | 必须按实际 RM0090 mapping 复核 |
| SPI2_TX DMA | DMA1 Stream4 Channel0 候选 | 若首版只收，也建议返回状态字节 |
| TRACE_READY | 一个独立 GPIO | 高=TraceBox 有 ingress 空间；引脚待资源检查后冻结 |

由于 SPI slave 不能主动产生时钟，Mini-DCM master 只有在 READY 有效时才拉低 NSS 并发送固定
帧。READY 不是“性能优化”，而是显式背压契约。

### 23.3 推荐 32 字节 TraceFrame

为了今晚尽快闭环，首版不要做可变长协议：

| Offset | Size | 字段 |
|---:|---:|---|
| 0 | 2 | magic，例如 `0x54 0x46` (`TF`) |
| 2 | 1 | version=1 |
| 3 | 1 | frame_type |
| 4 | 1 | source_node |
| 5 | 1 | severity |
| 6 | 2 | event_id |
| 8 | 4 | sequence |
| 12 | 4 | timestamp_ms，由 Mini-DCM master 提供 |
| 16 | 1 | payload_length，0..11 |
| 17 | 11 | payload |
| 28 | 4 | CRC-32，覆盖 0..27 |

这里选择固定 32 B 的原因：DMA NDTR 固定、NSS 边界明确、无需 ISR 解析长度、PC 易于构造测试
帧，单帧能携带一个结构化事件。后续确有需要再设计 fragmentation，不要在 V2 MVP 首版加入。

### 23.4 Mini-DCM 侧 producer

业务模块只提交结构化值，不直接操作 SPI：

```c
typedef enum {
    TRACE_EVT_SESSION_CHANGED = 0x0101,
    TRACE_EVT_SECURITY_RESULT = 0x0102,
    TRACE_EVT_DTC_CHANGED     = 0x0201,
    TRACE_EVT_CAN_BUS_OFF     = 0x0301,
    TRACE_EVT_QUEUE_OVERFLOW  = 0x0401
} TraceEventId;

TbStatus TraceHook_Emit(uint16_t event_id,
                        TraceSeverity severity,
                        const void *payload,
                        uint8_t payload_length);
```

`TraceHook_Emit()` 只复制到本地固定 ring。SPI sender 在 Mini-DCM 的正常前台任务/MainFunction
中取帧，看到 TRACE_READY 后启动 master DMA。CAN ISR、CanTp 状态机和 UDS handler 都不应该
等待 SPI。

### 23.5 TraceBox 侧 SPI slave RX DMA

推荐两个 32 B ingress buffer 做 ping-pong：

```c
on_spi2_nss_or_dma_start:
    arm_rx_dma(current_buffer, 32)

on_spi2_rx_dma_tc_irq:
    clear_flags()
    publish {buffer_index, local_rx_cycles, hardware_status}
    switch/arm_other_buffer()
    if no free ingress buffer:
        READY = 0
```

主循环执行：

```c
while (ingress_descriptor_available()) {
    frame = completed_buffer(index)
    if (!magic/version/length/crc_valid(frame)) {
        metrics.frame_rejected++
        continue
    }
    if (frame.sequence != expected_sequence[source]) {
        metrics.sequence_gap++
    }
    encode_trace_event_page(frame)
    publish_to_existing_page_pool()
    metrics.frame_accepted++
}
```

ISR 仍不解析 event_id、不算 CRC、不写 NOR。V1 的中断原则完整保留。

### 23.6 如何复用现有存储页

V2 页仍保留外层 `TBP1`、version、page_sequence、page CRC、capture_id、reserved 和 commit，
但使用新的 `record_type=2`。最快闭环方案是“一 TraceFrame 一物理页”：空间利用率不高，却能
最大限度复用两阶段提交、恢复和 dump。

完成后再优化为一页聚合 6 个 32 B frame：

```text
32 B common page header + 6 * 32 B frame + 31 B reserve + 1 B commit = 256 B
```

聚合版需要解决稀疏事件 flush timeout、partial page event_count 和跨来源时间排序，因此不建议
作为今晚第一个 V2 gate。

### 23.7 V2 最小验收

1. Mini-DCM master 发送 1 个固定测试事件，TraceBox 收到并落盘；
2. 连发至少 10,000 frame，frame/page sequence gap=0、CRC error=0、ingress/page overflow=0；
3. 主动发 1 个坏 CRC，TraceBox 拒绝且计数 +1；
4. 故意拉低 READY 或暂停 writer，master 不越权发送，恢复后继续；
5. 记录过程中按 RST/断电，恢复后只导出已提交事件页；
6. PC 将 session/security/DTC/CAN/queue 事件解码成按 timestamp 排序的时间线；
7. 保存串口日志、导出文件、帧计数和接线照片。

只有这些完成，V2 才能进入简历完成项。

## 24. 推荐的源码学习顺序

不要从 2,000 行命令 App 开始。按依赖由底向上阅读：

1. `Platform/Inc/tracebox_types.h`：先记错误语义；
2. `TraceCore/Src/tracebox_page_pool.c`：理解所有权状态；
3. `TraceCore/Src/tracebox_page_codec.c`：理解持久化协议；
4. `Platform/Src/tracebox_crc.c`：理解硬件 CRC 数据口径；
5. `Platform/Src/tracebox_adc_dma.c`：跟着 init/start/IRQ/pop 顺序；
6. `Platform/Src/tracebox_spi.c`：先 Polling，再 DMA start、ISR、MainFunction、recover；
7. `Storage/Src/tracebox_nor.c`：对照 phase enum 画 WREN/WIP 图；
8. `Recorder/Src/tracebox_recorder.c`：从 public API 跳到 callback，再看 producer/writer；
9. `Core/Src/main.c` 与 `Core/Src/stm32f4xx_it.c`：把模块放回运行时；
10. `tools/record_codec.py`、`dump_validate.py`、`tracebox_cli.py`：理解独立证据；
11. 最后看 `App/Src/bringup_app.c` 和 `web-console/` 的编排。

每读一层都回答四个问题：它拥有什么状态？输入 buffer 归谁？完成如何通知？出错后谁负责
cleanup？能回答这四个问题，就不是只会复述代码。

## 25. 常见面试问题与回答要点

### Q1：为什么不用 HAL SPI DMA？

不是因为 HAL 不能用，而是项目目标就是证明寄存器理解和异常路径。手写驱动可以明确展示
Stream mapping、EN 清零、flag 清除、RX 先启、BSY drain、CS BSRR、deadline 和 abort 后外设
复位。时钟与非核心基础仍使用 HAL，边界是有意识选择的。

### Q2：DMA 是否等于零 CPU？

不是。CPU 仍负责 descriptor 校验、寄存器启动、ISR、状态机 service、页打包、CRC 和 callback。
DMA 的价值是把逐字节搬运和整个线速等待从前台移走。项目用 302-cycle launch 和 23,881 次
并发前台循环证明“异步”，没有把它夸大成完整 CPU utilization。

### Q3：为什么 ADC 只有两个 buffer，描述符却有四个？

两个 buffer 是实际数据存储；四项 ring 是完成事件队列和丢事件检测。ring 不能让旧 M0/M1
内容活得更久，前台仍必须在 DMA 回到同一 bank 前完成快照。

### Q4：为什么不让 DMA 直接把 ADC 写进 Flash page？

ADC DMA 每次只产生 192 B samples，而持久化页还需要 header、CRC 和 commit；Flash 也不是
memory-mapped SRAM，需要命令/地址/WIP。一次 192 B RAM 快照隔离 DMA buffer 生命周期和
Flash 延迟，实测 pack 最大约 31 μs，成本可接受。

### Q5：八页池满了怎么办？

返回明确 OVERFLOW、计数并停止本次记录，不覆盖旧 READY/WRITING 页。池只能吸收短抖动，
不能解决平均消费者速率不足。正式测试 HWM=1，说明当前平均链路有余量。

### Q6：CRC 通过为何还需要 commit？

CRC 表示内容一致性；commit 表示写入协议已完成。body 和 commit 分开 program，掉电残页保持
commit=FF，恢复不会把它当作历史。两者一起才能表达可靠提交。

### Q7：为什么恢复遇到第一坏页就停？

V1 是从固定地址开始的线性 append-only session，有效数据按定义是连续前缀。继续搜索会增加
把旧数据或随机字节误认成当前 session 的风险。循环覆盖和 checkpoint 属于 V2。

### Q8：怎样证明 60 秒没有丢样？

MCU 侧检查 ADC descriptor sequence、packed/committed/verified 数量和各 overflow/error；PC 侧
对 1,600,000 raw bytes 的 6,250 页逐页验证 commit、CRC、sequence、capture_id，并重建
600,000 sample/59.9999 s。两侧证据相互独立。

### Q9：状态机 callback 会不会在 ISR 中执行？

不会。DMA ISR 只置 event。`TbSpi_MainFunction()` 看到 RX/TX done、确认 BSY 清零并 cleanup 后
才在前台执行 callback；NOR callback 推进 Recorder，也都发生在主循环上下文。

### Q10：当前最大不足是什么？

V1 是线性预擦除窗口，没有 A/B metadata、循环覆盖和磨损均衡；3 次真实物理掉电已经通过，
逻辑波形和网页鼠标点击视觉冒烟仍待补，但 localhost HTTP 到真实硬件和 artifact 下载已经
通过。V2 三板输入尚未实测。主动说明边界比把 MVP 包装成通用存储系统更可信。

## 26. 简历和口述建议

### 26.1 30 秒项目介绍

> Mini-DCM 更偏协议栈和双节点通信，所以第二个项目我专门补 MCU 底层。我用 STM32F407
> 裸机实现了 Timer 触发 ADC、DMA 双缓冲、固定页池以及 SPI DMA 到 W25Q64 的异步链路。
> Flash 页采用 CRC 加独立 commit byte，掉电后只恢复连续有效前缀。真实硬件连续记录 60 秒、
> 60 万点和 6,250 页，所有序号、CRC、DMA 和溢出错误为 0；PC 端再独立校验 1.6 MB dump。
> 我还做了 Polling/DMA 基准和 SPI/NOR timeout 恢复，V2 会把 Mini-DCM 的结构化事件通过
> SPI Slave DMA 接进来，形成三板黑匣子。

### 26.2 三条简历描述

可直接使用 `docs/08_original_requirements_traceability.md` 第 8.2 节的三条 V1 描述。若一页简历
空间紧，可压缩成：

- 裸机寄存器级实现 TIM2-ADC1-DMA2 双缓冲与 8×256 B 静态页池，实机 10 kS/s 连续采集
  60 s/600,000 点，HWM=1，DMA/overrun/overflow=0；
- 实现 SPI1 TX/RX DMA 与 W25Q64 WEL/WIP 异步状态机，覆盖 CS/BSY、timeout 和外设复位恢复；
  4 KiB Polling/DMA 实测 149,747/164,060 B/s，DMA 302 cycles 返回；
- 设计 CRC + commit 两阶段持久化页与连续前缀恢复，完成 6,250 页全读回和 1.6 MB PC 校验，
  3 次真实掉电恢复 2,035/2,902/3,581 页且 CRC/序号错误为 0，确定性 torn-page 拒绝 1 页
  残页，配套 Python/localhost 证据工具。

### 26.3 V2 完成后替换哪一条

如果三板联调证据完整，可把第三条扩展为：

- 设计 SPI2 Slave RX DMA 固定帧入口，将 Mini-DCM 的 session/DTC/CAN error/queue fault 事件
  跨板送入独立 TraceBox，通过 READY 背压、frame/page 双层序号与 CRC、NOR commit 页完成
  持久化和 PC 时间线重建；实测帧数、吞吐和错误计数填入最终结果。

不要先填虚构数字。完成 V2 后从实际 CSV/日志中替换“实测帧数、吞吐和错误计数”。

## 27. 当前证据索引

- 正式硬件报告：`docs/2026-09-14_final_hardware_acceptance.md`；
- 三次物理掉电：`docs/evidence/2026-09-14_power_cut_recovery.csv` 与
  `docs/2026-09-14_power_cut_campaign.md`；
- 60 秒 normalized metrics：`docs/evidence/2026-09-14_final_recorder_hardware.csv`；
- Polling/DMA：`docs/evidence/2026-09-13_spi_polling_dma_benchmark.csv`；
- ADC DMA 专项：`docs/evidence/2026-09-13_adc_dma_hardware.csv`；
- SPI timeout recovery：`docs/evidence/2026-09-13_spi_dma_recovery.csv`；
- NOR 状态机：`docs/evidence/2026-09-13_nor_selftest.csv`；
- RAM ring/page pool/codec：`docs/evidence/2026-09-13_ram_record_core.csv`；
- 原始日志和大文件 hash：`artifacts/manifest.md`；
- 固件候选：`firmware/releases/TraceBox_Final_v1.0.0.hex`。

## 28. 最后总结

这个项目最值得学习的不是某个寄存器值，而是贯穿全链路的四个原则：

1. **时间确定性**：让 Timer 和 DMA 决定数据产生/搬运，不让软件循环控制采样节奏；
2. **所有权确定性**：每块 RAM、每笔 DMA、每个 Flash page 在任何时刻只有一个明确 owner；
3. **状态确定性**：所有等待、错误和恢复都有 state、deadline 和 typed result；
4. **证据确定性**：MCU 指标、Flash 格式、PC 独立校验和 raw artifact 能互相验证。

V1 已经完整体现这四点。V2 的正确方向不是重写它，而是把 Mini-DCM 的结构化事件作为新
producer 接进同一套可靠管线，让两个项目从“简历上的两个条目”变成一个可联调的系统故事。
