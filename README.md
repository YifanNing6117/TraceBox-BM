# TraceBox-BM

基于 STM32F407VET6 的裸机数据采集与事件记录器。定时器触发 ADC，DMA 双缓冲接收采样，主循环将数据封装成 256 字节页，再通过异步 SPI DMA 写入 W25Q64 NOR Flash。序号、CRC 和单独写入的提交字节用于识别有效页与中断写入的尾页。

适合学习寄存器级 DMA、静态缓冲区所有权、异步驱动、背压和掉电恢复。V2 可通过独立 SPI2 链路接收 Mini-DCM 的结构化事件；基础 ADC 记录可以独立运行。

## 技术栈与项目重点

TraceBox-BM 的重点是**裸机环境下的数据搬运、缓冲区管理与可靠记录**。定时器决定采样节奏，DMA 在外设与内存之间搬运数据，主循环推进 Flash 状态机并管理数据页。理解这些职责后，可以沿着“采集 → 缓冲 → 写入 → 恢复 → 导出”的顺序阅读源码。

| 技术 | 在本项目中的用途 |
|---|---|
| C、STM32F407、CMSIS / 寄存器访问 | 实现 TIM、ADC、DMA、SPI、GPIO、CRC 与 DWT 关键路径 |
| STM32 HAL | 基础初始化、系统时钟和毫秒时基；关键数据通路由项目驱动控制 |
| TIM2 TRGO、ADC1、DMA 双缓冲 | 由硬件定时触发采样，在两块 96 点缓冲区之间交替接收 |
| SPI1 TX/RX DMA、W25Q64 NOR | 异步执行擦除、页写入、读回与完成状态查询 |
| SPI2 Slave RX DMA、READY 背压 | 接收 Mini-DCM Tester 的事件，并按可用缓冲区控制入口流量 |
| 静态环形队列、页池、状态机 | 明确缓冲区所有权，处理忙、超时、溢出与恢复 |
| CRC-32/MPEG-2、序号、提交字节 | 识别数据损坏、序号不连续与未提交尾页 |
| DWT 周期计数器 | 测量驱动与中断路径的 CPU 周期开销 |
| Python、pyserial、unittest | 串口命令、二进制页解析、CRC 校验、CSV 导出与离线测试 |
| Node.js、原生 HTML / CSS / JavaScript | 提供可选 localhost 控制页，复用同一 Python CLI |
| Keil MDK5 / ARMCC5、ST-Link | 固件编译、下载与调试 |

### 前置知识与学习顺序

| 顺序 | 需要掌握的知识 | 在项目中对应的阅读目标 |
|---|---|---|
| 1 | C 指针、结构体、位运算、`volatile` 与大小端 | 理解寄存器读写、页格式和共享状态；`volatile` 本身不保证原子性 |
| 2 | STM32 时钟树、GPIO、UART、中断与 SWD | 先跑通串口 `ping/info`，检查时钟与板级接线 |
| 3 | 定时器触发、ADC、DMA Stream / Channel 与双缓冲 | 理解 10 kHz 触发、M0/M1 交替和完成事件 |
| 4 | SPI 时序、全双工、片选与 NOR 的页/扇区约束 | 理解 WREN、WIP、先擦后写与跨页边界处理 |
| 5 | 非阻塞状态机、生产者/消费者、队列和缓冲区所有权 | 跟踪一块采样缓冲如何变成记录页，再交给 Flash 消费者 |
| 6 | CRC、二进制序列化、断电中断写入的后果 | 理解为什么需要完整性校验与独立提交字节 |
| 7 | Python 串口读写、文件解析和基本测试 | 在无硬件环境先运行 codec 单测与全规模离线导出 |

基础 ADC 记录路径可以独立学习。三板事件记录需要额外了解 Mini-DCM 的 CAN/UDS 请求链、SPI2 连线和 READY 信号，无需先掌握全部诊断服务。

## 裸机运行方式与记录逻辑

当前 TraceBox 固件**未使用 RTOS**，采用超级循环与中断协作。主循环依次调用 SPI、SPI 事件入口、NOR、ADC、Recorder 和命令处理模块的 `MainFunction`。中断确认硬件状态并发布最少的事件或元数据；数据封装、CRC 计算、Flash 操作推进及 UART 命令处理在主循环中完成。入口见 [main.c](firmware/TraceBox_F407/Core/Src/main.c)。

ADC DMA 在两块采样缓冲区间交替工作。主循环消费完成描述符，将一块 96 点数据封装为 256 字节记录页，再从静态页池交给异步 NOR 驱动。页池让采样生产者与 Flash 消费者在明确的所有权转换下协作，队列高水位和溢出计数用于观察消费者是否跟得上生产者。V2 则从 SPI2 接收固定长度事件帧，经过事件与记录页两层 CRC 校验逻辑完成记录和导出。

写入时先写页内容，再单独写最后一个提交字节。恢复扫描结合提交状态、CRC 和序号判断有效前缀及异常尾页，避免把中断写入的数据当作完整记录。当前采用预擦除、写满即停止的线性窗口；通用文件系统和全盘磨损均衡不在当前实现范围。格式与恢复说明见 [记录格式文档](docs/history/04_record_format_and_recovery.md)。

与 Mini-DCM 配合时，Mini-DCM 的 FreeRTOS 任务负责诊断业务与事件发送，TraceBox 的裸机循环负责独立接收和持久化。两个项目分别展示任务调度/协议处理，以及 DMA/缓存/存储状态机。

```text
TIM2 → ADC1 → DMA 双缓冲 → 静态页池 → SPI1 DMA → W25Q64
                                      ↑
Mini-DCM Tester → SPI2 RX DMA / READY ──┘
                                      ↓
                              UART → Python → CSV
```

## 硬件与工具

| 项目 | 配置 |
|---|---|
| MCU | LXB407VE-P1 / STM32F407VET6，8 MHz HSE，168 MHz 系统时钟 |
| Flash | W25Q64；3.3 V 供电 |
| Flash SPI1 | PB0 CS、PA5 SCK、PA6 MISO、PA7 MOSI |
| ADC | PA0 / ADC1_IN0，TIM2 10 kHz，96 点/块 |
| UART | PA9 TX、PA10 RX，115200 8N1，3.3 V TTL |
| 固件构建 | Keil MDK5 / ARMCC 5，DFP `Keil.STM32F4xx_DFP.2.9.0` |
| 主机 | Python 3.10+，pyserial；可选 Node.js 18+ |

SWD、电源和接线细节见 [板级说明](firmware/TraceBox_F407/README.md)。板上 USB-C 不等于 USART1 转串口。首次连接 Flash 前移除 PA7 与 PA6 的 SPI 回环跳线；已由 USB 供电时，勿再把调试器的 3.3 V 输出接到板卡电源。

## 无硬件验证

在仓库根目录执行：

```sh
python -m venv .venv
# 激活 .venv 后执行；Windows: .venv\Scripts\Activate.ps1
# Linux/macOS: . .venv/bin/activate
python -m pip install -r tools/requirements.txt
python -m unittest discover -s tests -v
python tools/offline_full_scale_test.py
node --test web-console/test/server.test.js
python tools/audit_publication.py
```

全规模离线测试生成并解析 6,250 页、600,000 点合成数据，输出写入临时目录。该测试验证 PC 编解码与导出，硬件采样率和掉电行为需要连接实板复测。

## 构建与下载

使用 Keil 打开 `firmware/TraceBox_F407/MDK-ARM/TraceBox_F407.uvprojx`。命令行干净构建需要显式传入本机 Keil 路径：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_keil.ps1 -KeilExe 'C:\Keil_v5\UV4\UV4.exe'
```

该脚本生成 `firmware/releases/TraceBox_Final_v1.0.0.hex`，输出 SHA-256。文件名沿用已有构建脚本，当前源码包含 V2 事件入口。本仓库分发源码，HEX/BIN 由本地构建生成。

安装 STM32CubeProgrammer，将 `STM32_Programmer_CLI.exe` 加入 PATH，确认连接目标后下载：

```powershell
powershell -ExecutionPolicy Bypass -File tools/flash_stlink.ps1 -CheckOnly
powershell -ExecutionPolicy Bypass -File tools/flash_stlink.ps1
```

## 记录与导出

基础板级检查：

```sh
python tools/board_smoke_test.py --port COM9
python tools/tracebox_cli.py --help
```

以下命令会擦写 Flash 的记录区域，请先备份所需数据，并确认串口对应 TraceBox：

```sh
python tools/tracebox_cli.py full --port COM9 --seconds 60 --dump-baud 460800
```

日志和导出结果写入 `artifacts/raw`、`artifacts/exports`，均已加入 Git 忽略规则。串口示例按 Windows 编写；localhost 控制页当前只接受 COM 端口。

## 可选控制页与三板联调

```sh
node web-console/server/src/server.js --port 8765
```

打开 `http://127.0.0.1:8765`。服务仅监听回环地址。若 Python 不在 PATH，设置 `TRACEBOX_PYTHON` 为解释器路径。控制页复用 Python CLI，执行单一串口任务并显示结果。

将 [Mini-DCM](https://github.com/YifanNing6117/Mini-DCM) 放在并列的 `../Mini-DCM` 目录。Mini-DCM 公开工程中的 Tester 已集成事件发送，通常无需运行 `tools/sync_minidcm_tester_v2.ps1`；该脚本仅用于旧工程迁移。其他布局可通过 `MINIDCM_BRIDGE` 环境变量指定 `serial_bridge.py` 的路径。

三板连线和事件格式见 [V2 设计](docs/history/10_v2_minidcm_spi_integration.md)。确认两块 F407 的 SPI2 / READY 连线后运行：

```sh
python tools/tracebox_cli.py trace-demo --port COM9 --tester-port COM10 --frames 8 --dump-baud 460800
```

## 文档、验证与许可证

`docs/history` 保留架构、引脚、记录格式、完整学习指南和 V2 集成记录。历史路径和链接可能指向开发阶段未公开的原始证据；当前运行命令以本 README 为准。私人交接、采购、日志、截图和设备 UID 未纳入发布。

本次快照的测试和构建结果见 [验证记录](docs/VALIDATION.md)。此前硬件记录描述了 60 秒采集和三次掉电恢复；本次整理未连接硬件复测，不将历史数字作为本次新测量。

自有代码与文档采用 [MIT](LICENSE)，第三方文件保留原许可证，见 [第三方说明](THIRD_PARTY_NOTICES.md)。
