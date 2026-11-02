> 历史设计记录：原开发目录中的路径、阶段目标与实验记录按当时状态保留。当前构建、运行和验证入口见[仓库 README](../../README.md)。部分原始日志与规划资料未纳入公开快照。

# TraceBox-RT V2：Mini-DCM 三板 SPI/DMA 事件黑匣子

## 1. V2 的定位

V1 已经证明 STM32F407 裸机 ADC/DMA 双缓冲、寄存器级 SPI1/DMA2、W25Q64
异步状态机、页缓冲池、CRC 提交页、掉电恢复和 PC 导出。V2 不再增加一个课程式
传感器读取，而是把 TraceBox 变成 Mini-DCM 之外的独立基础设施节点：Tester 和 ECU
继续完成诊断业务，TraceBox 通过独立 SPI 链路异步接收结构化运行事件并可靠落盘。

因此两个项目之间的关系是“被观测系统 + 外置记录基础设施”，而不是第三块板参与
UDS 服务处理。Mini-DCM 展示协议栈、任务与队列；TraceBox 展示寄存器级数据搬运、
缓存所有权、背压、持久化和故障隔离。

## 2. 实际硬件拓扑

```text
PC --UART 9600--> F407 Tester --CAN 500 kbit/s--> F103 ECU + MPU6050
                           |
                           | SPI2 master + DMA1 + PE1 READY input
                           v
PC <--UART 115200-- TraceBox F407 SPI2 slave RX DMA
                           |
                           | SPI1 master + DMA2
                           v
                        W25Q64
```

板间连线：

| 信号 | Mini-DCM F407 Tester | TraceBox F407 | 方向 |
|---|---|---|---|
| NSS/CS | PB12 | PB12 | Tester -> TraceBox |
| SCK | PB13 | PB13 | Tester -> TraceBox |
| MISO | PB14 | PB14 | TraceBox -> Tester，当前只发送填充值 |
| MOSI | PB15 | PB15 | Tester -> TraceBox |
| READY | PE1 输入 | PE0 输出 | TraceBox -> Tester |
| GND | GND | GND | 必须共地 |

TraceBox 自己的 Flash 仍使用 SPI1：PB0 CS、PA5 SCK、PA6 MISO、PA7 MOSI。
两条 SPI 总线物理独立，SPI2 负责入口，SPI1 负责存储。

## 3. 32 字节 TraceFrame

所有字段显式小端，固定长度便于 DMA，不允许动态内存。

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 2 | magic `T`,`F` |
| 2 | 1 | version = 1 |
| 3 | 1 | event type |
| 4 | 1 | source：Tester/ECU |
| 5 | 1 | severity |
| 6 | 2 | event_id |
| 8 | 4 | source sequence |
| 12 | 4 | timestamp_ms |
| 16 | 1 | payload length，最大 11 |
| 17 | 11 | payload/零填充 |
| 28 | 4 | CRC32/MPEG-2，覆盖字节 0..27 |

当前事件包含 BOOT、CAN_TX、CAN_RX、CAN_ERROR、TX_QUEUE_OVERFLOW 和
RX_QUEUE_OVERFLOW。来源是 ECU 的事件表示 Tester 在 CAN RX 边界观察到 ECU 帧；
时间戳也以 Tester 的接收时刻为准，因此无需让两个 MCU 先做时钟同步。

编码逻辑可概括为：

```c
TbStatus TraceFrame_Encode(const TraceEvent *event, uint8_t out[32])
{
    if ((event == NULL) || (out == NULL) || (event->payload_len > 11U)) {
        return TB_STATUS_BAD_ARGUMENT;
    }
    put_magic_version_type_source_severity(out, event);
    put_le16(&out[6], event->event_id);
    put_le32(&out[8], event->sequence);
    put_le32(&out[12], event->timestamp_ms);
    copy_and_zero_pad(&out[16], event->payload, event->payload_len);
    put_le32(&out[28], Crc32Mpeg2(out, 28U));
    return TB_STATUS_OK;
}
```

## 4. Tester：生产者、环形队列和 SPI2 DMA 主机

业务模块不直接操作 SPI。它只调用 `TraceLink_Publish()`，把事件编码进 32 槽静态
环形队列。队列满时返回显式错误并增加 `queueRejected`，不会覆盖旧事件。

```c
status = TraceLink_Publish(source, type, severity, event_id,
                           payload, payload_len, now_ms);
if (status == TRACE_LINK_OK) {
    notify_bridge_task();
}
```

桥接任务是唯一 SPI2 所有者。只有 `READY=1`、队列非空、状态为 IDLE 且满足上一笔
事务后的 1 ms 边界间隔时，才拉低 PB12 并启动 DMA1 Stream3/4。DMA 中断只清标志、
记结果和通知任务；CS 释放、队列出队、超时判断和恢复均在任务上下文完成。

状态机：

```text
IDLE --READY && pending--> DMA_ACTIVE
DMA_ACTIVE --RX/TX TC--> COMPLETE --> release CS --> pop --> IDLE
DMA_ACTIVE --TE/DME/timeout--> RECOVER --> disable streams/clear SPI --> IDLE
IDLE --READY=0--> keep frame in ring and count backpressure
```

直接模式下的 FIFO Error 标志不被当作致命错误；只处理 DME/TE。第一次联调中曾把
FEIF 误判为失败，导致同一个 BOOT 帧重复发送，实机计数器帮助定位后已修正。

## 5. TraceBox：SPI2 从机 RX DMA 和单信用背压

`TbSpiIngress` 在静态 32 字节缓冲上预装 SPI2 RX DMA。NSS 上升沿确认一笔物理事务
结束，DMA 中断只发布完成描述符和计数。主循环完成格式检查、CRC、序号处理及向
Recorder 交接。

READY 不是简单的“SPI DMA 空闲”，而是端到端缓存信用：

```text
Flash page pool available
  -> TraceBox raises READY (one credit)
  -> Tester consumes credit and starts one 32-byte DMA transaction
  -> TraceBox lowers READY, validates frame
  -> recorder programs body, verifies page and writes commit byte
  -> only then grant the next READY credit
```

初版在 DMA 描述符释放时立即重新拉高 READY，Flash 消费速度跟不上入口时页池曾发生
溢出。V2 将信用释放点移动到 Flash commit 完成之后，使背压覆盖整个
`SPI ingress -> page pool -> W25Q64` 管线。64 事件回归的 page-pool HWM 为 1，溢出为 0。

## 6. W25Q64 页面与恢复

每个 TraceFrame 当前占一个 256 字节页，复用 V1 的可靠页协议：页头包含 magic、版本、
record type、capture ID、page sequence 和 payload length；页内保存 TraceFrame；页级
CRC 覆盖提交前数据；偏移 255 的 commit 字节最后单独编程。

```text
TraceFrame CRC：证明 32 字节板间传输内容正确
Page CRC：证明打包和 Flash 中的持久化页面正确
Commit byte：区分完整页与掉电残页
Sequence：发现缺页、重排或重复
```

启动恢复只接受从数据区起点开始连续的 `commit + CRC + sequence` 有效前缀。V1 的
三次真实掉电和确定性残页测试仍直接适用于 V2 的页面层。

## 7. 主循环职责

TraceBox 不使用 RTOS，超级循环只推进有界状态机：

```c
for (;;) {
    TbUart_Service();
    TbSpiIngress_Service();
    if (TbSpiIngress_TakeFrame(frame)) {
        status = TraceFrame_DecodeAndCheck(frame, &event);
        if (status == OK) {
            TbRecorder_RecordTrace(&event);
        } else {
            count_rejection(status);
            TbSpiIngress_GrantCredit();
        }
    }
    TbRecorder_Service();
    TbNor_Service();
    TbSpiDma_Service();
    if (TbRecorder_PageCommitted()) {
        TbSpiIngress_GrantCredit();
    }
}
```

所有外设等待都有 deadline；ISR 不做 CRC、复制、Flash 命令、printf 或无限等待。

## 8. PC 与 localhost

`dump_validate.py` 自动区分 ADC 页和 TRACE 页。TRACE 模式先验证页 CRC，再验证内部
TraceFrame CRC，并输出时间线 CSV/JSON/Markdown、来源计数、事件类型计数及序号错误。

本地控制台只绑定 `127.0.0.1:8765`。V2 页面选择 COM9 TraceBox、COM10 Tester 和
目标事件数后，执行：

```text
trace prepare
-> 读取 Tester 积压深度
-> 启动 TraceBox trace record
-> Mini-DCM UDS stress 产生真实 CAN_TX/CAN_RX
-> 等待全部页 commit/verify
-> 二进制定长 dump
-> 双层 CRC 校验和时间线导出
```

Node 仅做白名单参数验证和单任务协调，串口及页面协议仍唯一实现在 Python CLI 中。

## 9. 真实硬件结果

固件：`TraceBox_RT_v2.0.0.hex`，SHA-256
`B1C37FF755419D543B890D42A9C576A3C9287814028CFC4140B42E8AD8A86445`。

- 64 事件：64/64 committed/verified，Tester 46、ECU 18，页 CRC、Trace CRC、序号、
  SPI/DMA/Flash 错误均为 0；页池 HWM 1；ISR max 176 cycles（约 1.05 us）。
- CRC 故障注入：1 个坏 TraceFrame 被拒绝并计为 trace CRC error，后续 4 个正常事件
  继续提交；PC 修改 dump 单 bit 后校验器返回 FAIL 并定位首个坏页。
- 一键 CLI：3/3 UDS 成功，6/6 事件提交与导出，全部错误为 0。
- localhost HTTP：2/2 UDS 成功，4/4 事件提交与导出，任务返回码 0。
- 主机测试：Python 21 passed，Node 5 passed。

规范化数据见 `docs/evidence/2026-09-15_tracebox_v2_hardware.csv`，原始本地文件见
`artifacts/manifest.md`。

## 10. 当前边界与后续扩展

- 一页一事件优先可恢复性和一周闭环，空间利用率不高；后续可做多事件聚合页。
- READY 是单信用，吞吐并未追求极限；后续可做多信用/批量 DMA，但必须保留所有权证明。
- 一键任务会优先消费 Tester 已积压事件，所以时间线可能包含任务开始前的事件；需要
  严格采集窗口时可增加“新 capture epoch/队列排空”协议。
- 尚缺逻辑分析仪物理波形截图；功能、错误计数和回读证据已经闭环，该截图只增强展示。
- 可继续增加 HardFault/看门狗复位上下文、寄存器快照、跨板时间同步和循环磨损管理。

## 11. 简历描述候选

- 基于 STM32F407 裸机实现外置运行黑匣子，设计 32 字节 TraceFrame 与
  SPI2 Slave RX DMA/READY 背压链路，将 Mini-DCM Tester/ECU 的 CAN 运行事件异步写入
  W25Q64；64 事件三板回归中 SPI/DMA/Flash/序号错误为 0，ISR 最长 176 cycles。
- 设计静态环形队列、单信用端到端背压和异步 Flash 状态机，解决入口快于落盘导致的
  page-pool overflow，并以双层 CRC、commit byte 和连续序号实现传输与持久化完整性验证。
- 开发 localhost 一键硬件回归与 Python 时间线导出，自动驱动 UDS/CAN 产生日志并完成
  预擦除、采集、回读和 CRC 校验；完成坏 CRC 隔离、bit-flip 检出及异常后继续记录验证。
