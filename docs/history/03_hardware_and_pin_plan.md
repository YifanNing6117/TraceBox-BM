> 历史设计记录：原开发目录中的路径、阶段目标与实验记录按当时状态保留。当前构建、运行和验证入口见[仓库 README](../../README.md)。部分原始日志与规划资料未纳入公开快照。

# 硬件、引脚与 DMA 资源计划

## 1. 目标板

- 核心板：LXB407VE-P1，STM32F407VET6，LQFP100。
- 第一阶段由 USB Type-C 仅供电；板载 Type-C 的 D+/D- 直连 PA12/PA11，不是 USB 转串口。
- 板载 8 MHz HSE、32.768 kHz LSE。
- 板载蓝色用户 LED 位于 PC0，电路为 3.3 V -> 1 kΩ -> LED -> PC0，因此**低电平点亮**。
- 调试接口：PA13/SWDIO、PA14/SWCLK、NRST、GND。
- 串口接口：USART1 PA9/TX、PA10/RX。

## 2. V1 引脚分配

| 功能 | MCU 引脚 | 配置 | 说明 |
|---|---|---|---|
| 状态 LED | PC0 | GPIO output | active-low，不再作为 ADC 输入 |
| USART1 TX | PA9 | AF7 | 接 USB-UART RX；bring-up 115200，P0 dump 460800 |
| USART1 RX | PA10 | AF7 | 接 USB-UART TX；3.3 V TTL |
| ADC 输入 | PA0 / ADC1_IN0 | Analog | 电位器中点或安全模拟信号 |
| SPI1 SCK | PA5 | AF5 | 接 Flash CLK |
| SPI1 MISO | PA6 | AF5 | 接 Flash DO/IO1 |
| SPI1 MOSI | PA7 | AF5 | 接 Flash DI/IO0 |
| Flash CS# | PB0 | GPIO output | BSRR 控制，默认拉高 |
| 测量脉冲 | PB1 | GPIO output | ISR/transaction timing，可选 |
| V2 TRACE_READY | 待定 | GPIO/EXTI | V1 不占用协议语义 |

PA11/PA12 留给 USB，PB3/PB4/PB5 可能与板载 LCD FPC 复用，因此 V1 选择 SPI1。

## 3. DMA 分配

依据 STM32F407 RM0090 的 DMA2 request mapping，V1 固定以下组合：

| 请求 | DMA | Stream | Channel | 模式 |
|---|---|---:|---:|---|
| ADC1 | DMA2 | 4 | 0 | peripheral-to-memory, circular, DBM |
| SPI1_RX | DMA2 | 2 | 3 | peripheral-to-memory, normal |
| SPI1_TX | DMA2 | 3 | 3 | memory-to-peripheral, normal |

这样三条关键链路不共享 Stream。USART1 在 V1 使用中断/轮询输出，避免占用 DMA2 资源。
正式配置前仍需用实际芯片版本的 RM0090 Table 44 复核。

## 4. Flash 接线

裸芯片/标准模块的最小连接：

| Flash | 接到核心板 | 约束 |
|---|---|---|
| VCC | 3V3 | 禁止 5 V |
| GND | GND | 与 ST-Link、USB-UART 共地 |
| CLK | PA5 | 首次 1.3125 MHz 或更低，逐级升频 |
| DO / IO1 | PA6 | MISO |
| DI / IO0 | PA7 | MOSI |
| CS# | PB0 | 建议 10 kΩ 上拉到 3V3 |
| WP# / IO2 | 3V3 | 10 kΩ 上拉，V1 不使用 Quad |
| HOLD# / IO3 | 3V3 | 10 kΩ 上拉，V1 不使用 Quad |

Flash 旁必须有 100 nF 去耦，推荐再加 1 μF。线长尽量小于 10 cm；SCK 出现过冲时在 MCU
端串 22~47 Ω。面包板阶段从低速开始，不以 Flash 标称最高时钟作为目标。

## 5. ADC 输入

最简单输入是 10 kΩ 电位器：两端接 3V3/GND，中点接 PA0。禁止输入负压或高于 VDDA。
如用 PWM + RC 产生波形，必须先测量电压范围。V1 不需要外部 ADC。

## 6. 上电与调试安全

1. 首次只接 USB-C 供电，测量 5 V 和 3.3 V，无异常发热。
2. ST-Link 接 GND、SWDIO、SWCLK、NRST；避免同时从多个接口向板卡强行供电。
3. USB-UART 使用 3.3 V TTL，交叉连接 TX/RX，并与板卡共地；先以 115200 验收，再测试
   460800 的 binary dump，若适配器不稳定则降到 230400。
4. Flash 接入前先通过 PA7->PA6 物理短接完成 SPI loopback。
5. 使用逻辑分析仪时所有设备共地；探头地线要短。

## 7. 采购验收规格

必须确认：

- W25Q64JV/W25Q128JV 或等价 NOR，供电和逻辑均为 2.7~3.6 V；
- 模块没有为 5 V Arduino 使用而串入过慢的双向电平转换器；
- 能获得明确料号和数据手册，支持 0x9F JEDEC ID、256 B Page Program、4 KiB erase；
- 若购买 SOP8 转接板，封装宽度匹配（常见 150 mil 与 208 mil 不能混买）；
- 逻辑分析仪至少 8 通道/24 MHz，并能由 sigrok/PulseView 解码 SPI。

购买具体型号后，把 JEDEC ID、容量、页/扇区大小、最大 program/erase 时间和状态寄存器位
从该型号数据手册录入配置，禁止只靠商品标题。
