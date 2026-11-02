# TraceBox-BM

基于 STM32F407VET6 的裸机数据采集与事件记录器。定时器触发 ADC，DMA 双缓冲接收采样，主循环将数据封装成 256 字节页，再通过异步 SPI DMA 写入 W25Q64 NOR Flash。序号、CRC 和单独写入的提交字节用于识别有效页与中断写入的尾页。

适合学习寄存器级 DMA、静态缓冲区所有权、异步驱动、背压和掉电恢复。V2 可通过独立 SPI2 链路接收 Mini-DCM 的结构化事件；基础 ADC 记录可以独立运行。

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
