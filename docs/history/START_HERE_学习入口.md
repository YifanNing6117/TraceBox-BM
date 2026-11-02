> 历史设计记录：原开发目录中的路径、阶段目标与实验记录按当时状态保留。当前构建、运行和验证入口见[仓库 README](../../README.md)。部分原始日志与规划资料未纳入公开快照。

# TraceBox-RT V2 学习入口

这份文件是项目的唯一学习起点。现在不需要连接板卡，也不要先钻进 HAL 或 CMSIS
驱动目录。先建立整条数据链的心智模型，再按模块阅读源码。

## 1. 先用一句话讲清项目

TraceBox-RT 是一个运行在 STM32F407 上的裸机运行时黑匣子：它既能用
TIM2 -> ADC1 -> DMA2 双缓冲采集本地数据，也能通过 SPI2 Slave RX DMA 接收
Mini-DCM Tester/ECU 的结构化事件；两类数据最终都经过静态缓冲、异步 SPI1 DMA、
W25Q64 提交页、CRC/序号校验和掉电恢复，再由 Python/localhost 导出证据。

## 1.1 学习环境

- 阅读源码：任意文本编辑器即可。
- 固件构建：Keil MDK5，目标 `STM32F407VETx`，DFP 版本要求见
  `firmware/TraceBox_F407/README.md`。
- PC 测试：Python 3 + `tools/requirements.txt`；当前电脑已验证环境为
  `python`。
- localhost：Node.js 18 或以上，无第三方 npm 依赖。
- 完整硬件：TraceBox F407、W25Q64、Mini-DCM Tester/ECU、CH340；逻辑分析仪非必需。

## 2. 推荐阅读顺序

### 第一遍：只理解项目价值，约 40 分钟

1. `README.md`：项目定位、V1/V2 边界和已完成结果。
2. `docs/01_requirements_and_scope.md`：为什么它不是“SPI 读 Flash”实验。
3. `docs/02_system_architecture.md`：分层、所有权和裸机执行模型。
4. `docs/10_v2_minidcm_spi_integration.md`：最终三板链路和实测数据。

这一遍结束后，应该能不看资料画出：

```text
Mini-DCM CAN event
  -> Tester event ring
  -> SPI2 master DMA / READY
  -> TraceBox SPI2 slave RX DMA
  -> recorder page pool
  -> SPI1 DMA / W25Q64
  -> UART dump / PC double-CRC validator
```

### 第二遍：学习 V1 的底层基础，约 3～4 小时

主教材是 `docs/09_complete_implementation_learning_guide.md`，源码按下面顺序读：

1. `firmware/TraceBox_F407/Core/Src/main.c`
   - 看启动顺序和超级循环。
   - 找出哪些模块由 HAL 初始化，哪些模块直接操作寄存器。
2. `Platform/Src/tracebox_adc_dma.c`
   - 看 TIM2 TRGO、ADC1、DMA2 Double Buffer、M0/M1 和 ISR 描述符发布。
3. `TraceCore/Src/tracebox_event_ring.c` 与 `tracebox_page_pool.c`
   - 看单调 head/tail、drop-newest、buffer ownership 和 high-water。
4. `Platform/Src/tracebox_spi.c`
   - 对比 Polling 与 DMA；重点看启动顺序、RX/TX 双 Stream 完成条件、BSY 尾部和 timeout recovery。
5. `Storage/Src/tracebox_nor.c`
   - 看 WREN、WEL、WIP 轮询、页边界、deadline 和回调。
6. `TraceCore/Src/tracebox_page_codec.c`
   - 看显式小端序列化，不要把 C 结构体直接当线格式。
7. `Recorder/Src/tracebox_recorder.c`
   - 最后再看这个组合层如何把 producer、page pool、NOR writer 和 recovery 串起来。

每读一个 `.c`，先读同名 `.h`，写下三件事：输入是什么、谁拥有 buffer、什么时候
归还所有权。答不出来就还没有真正看懂。

### 第三遍：学习 V2 的板间异步链路，约 2～3 小时

按数据方向阅读：

1. `integration/minidcm_tester/Inc/trace_link.h`
2. `integration/minidcm_tester/Src/trace_link.c`
3. `firmware/TraceBox_F407/Platform/Inc/tracebox_spi_ingress.h`
4. `firmware/TraceBox_F407/Platform/Src/tracebox_spi_ingress.c`
5. `firmware/TraceBox_F407/TraceCore/Src/tracebox_trace_frame.c`
6. `firmware/TraceBox_F407/Recorder/Src/tracebox_recorder.c` 中的
   `Recorder_ServiceTraceProducer()` 和 `TbRecorder_RecordTrace()`。

重点回答：

- 为什么 READY 必须在 Flash commit 后返还，而不能在 SPI DMA 完成后返还？
- Tester 队列满、TraceBox descriptor 满、page pool 满分别由谁发现？
- 为什么 DMA direct mode 的 FEIF 不能在这里直接等同于传输失败？
- 为什么 ISR 只发布事实，CRC、CS 清理和恢复都留给主上下文？
- 为什么采集开始后的第一个 sequence 只建立基准，而不是必须等于 0？

### 第四遍：学习证据链，约 1～2 小时

1. `tools/record_codec.py`：PC 端页格式与 CRC 真值。
2. `tools/dump_validate.py`：ADC/TRACE 自动识别和双层 CRC。
3. `tools/tracebox_cli.py`：有界串口操作和 V2 双串口编排。
4. `web-console/server/src/server.js`：命令白名单和 single-operation owner。
5. `docs/evidence/2026-09-15_tracebox_v2_hardware.csv`：最终量化结果。
6. `artifacts/manifest.md`：每项结论来自哪份原始证据。

## 3. 两条核心调用链

### V1 ADC 数据链

```text
TIM2 update
-> ADC1 conversion
-> DMA2 Stream4 writes M0/M1
-> DMA TC ISR publishes block descriptor
-> Recorder_ServiceAdcProducer
-> page encode + hardware CRC
-> page pool READY_TO_WRITE
-> Recorder_ServiceWriter
-> Nor_PageProgramAsync
-> SPI1 DMA
-> body verify
-> commit byte
```

### V2 事件数据链

```text
Mini-DCM CAN TX/RX boundary
-> TraceLink_EmitCan
-> Tester 32-slot ring
-> READY check
-> SPI2 master DMA
-> TraceBox DMA/NSS IRQ publishes descriptor
-> TbSpiIngress_PopDescriptor
-> TbTraceFrame_Decode + inner CRC
-> page encode + outer CRC
-> W25Q64 body/verify/commit
-> TbSpiIngress_GrantCredit
```

## 4. 必须掌握的五个不变量

1. 一个 buffer 同一时刻只有一个 owner。
2. 中断只确认硬件和发布最小事件，不做可变时长工作。
3. 每个异步硬件状态都必须有 deadline 和可恢复错误码。
4. `commit=valid` 必须最后写；CRC 通过不能替代提交标记。
5. READY 表示端到端可接收能力，不只是某个 DMA buffer 空闲。

## 5. 现在断开硬件也能做的练习

在项目根目录执行：

```powershell
python -m pytest -q
Set-Location .\web-console
npm test
```

预期分别是 `21 passed` 和 `5 passed`。建议再完成以下练习：

- 在纸上画出 `TbRecorderState` 和 `TraceLinkState`，标明每条错误转移。
- 手算一个 32 字节 TraceFrame 的各字段偏移，解释为什么 payload 只有 11 字节。
- 找出所有 `volatile` 变量，判断它是否真的由 ISR 和主上下文共享。
- 找出所有 high-water/overflow/recovery 计数器，并说明每个计数器证明什么。
- 用 PC 单测新增一个 payload 长度 12 的非法 TraceFrame 用例，预期必须拒绝。

## 6. 接回硬件后的最短验收

接线、串口和注意事项见 `docs/10_v2_minidcm_spi_integration.md`。三板连接完成后：

```powershell
python .\tools\tracebox_cli.py trace-demo `
  --port COM9 --tester-port COM10 --frames 8 --dump-baud 460800
```

或者启动 `web-console/start.ps1`，访问 `http://127.0.0.1:8765`，选择 COM9、COM10，
点击“运行 V2 三板闭环”。成功标准不是“灯亮”，而是 UDS 成功、目标事件全部
committed/verified、两层 CRC 和所有 DMA/SPI/Flash/sequence 错误为 0。

## 7. 面试复述顺序

建议按“问题—设计—踩坑—证据”讲：

1. 问题：Mini-DCM 已有协议与 RTOS，但缺独立、掉电可恢复的底层运行记录能力。
2. 设计：第三块 F407 裸机 TraceBox，SPI2 DMA 输入、SPI1 DMA 存储、固定缓冲和状态机。
3. 踩坑：FEIF 误判、READY 只覆盖入口导致页池溢出、采集边界 stale credit。
4. 修复：错误分类、Flash commit 后返还单信用、边界清理和 1 ms 事务间隔。
5. 证据：64/64 三板事件、错误为 0、page-pool HWM 1、ISR max 176 cycles、CRC 注入可隔离。

不要说“DMA 不占 CPU”“绝对不会丢数据”或“实现了通用文件系统”。准确说法是：
DMA 降低逐字节搬运占用；当前固定容量和背压策略下实测无溢出；存储格式是针对顺序
事件记录优化的 append-only 提交页。

## 8. 哪些文件先不要看

- `Drivers/`：第三方 CMSIS/HAL，不是本项目设计重点。
- 旧版 `firmware/releases/*.hex`：用于阶段留档，不用于读源码。
- `artifacts/raw`、`artifacts/exports`：本机生成证据，先看规范化 CSV 和 manifest。
- `docs/09` 第 23 节中的 V2“建议方案”：它是实施前设计；最终实现以 `docs/10` 为准。

## 9. 当前冻结版本

- 功能基线 commit：`b0f45d2`；完整学习包使用 Git tag `v2.0.0`。
- TraceBox 固件：`firmware/releases/TraceBox_RT_v2.0.0.hex`。
- SHA-256：`B1C37FF755419D543B890D42A9C576A3C9287814028CFC4140B42E8AD8A86445`。
- 详细研发过程：`docs/process/2026-09-15_v2_minidcm_spi_ingress.md`。
- 当前状态：核心功能完成；逻辑分析仪波形、事件聚合页、capture epoch 属于可选扩展。
