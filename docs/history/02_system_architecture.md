> 历史设计记录：原开发目录中的路径、阶段目标与实验记录按当时状态保留。当前构建、运行和验证入口见[仓库 README](../../README.md)。部分原始日志与规划资料未纳入公开快照。

# 系统架构

## 1. 分层边界

```text
Application
  TraceBox_App / CLI / Demo Controller / Fault Injection
                         |
Services                 v
  Acquisition_Service / Recorder_Service / Metrics / Recovery
                         |
Core                     v
  Page Pool / SPSC Index Ring / Record Codec / Deadline Utility
                         |
Async Drivers            v
  Nor_Flash FSM / Spi_Bus FSM / AdcDma / Timebase / CRC
                         |
MCAL/BSP                  v
  RCC, GPIO, TIM2, ADC1, DMA2, SPI1, USART1, DWT, NVIC registers
```

约束：应用层不直接读写 SPI/ADC/DMA 寄存器；NOR 驱动不访问采样缓冲；ISR 不调用
Recorder、CRC、UART 或 NOR 状态机。

## 2. 裸机执行模型

系统没有任务和阻塞式调度。主循环按事件推进短小状态机：

```text
IRQ publishes event/counter
          |
          v
atomic event snapshot
          |
          v
Acquisition_Run -> Recorder_Run -> Nor_Run -> SpiBus_Run
          |
          +----> Metrics_Run -> Cli_Run -> Watchdog_Run
```

每个 `*_Run()` 必须在有限时间内返回，不允许等待 Flash WIP、DMA 完成或串口发送。
等待通过状态 + deadline 表达，下次循环继续推进。CPU 空闲时可执行 `__WFI()`；V1 的性能
测试同时记录 occupied cycles，避免把 DMA 等待时间误算成 CPU 占用。

## 3. 事件发布

建议使用每个中断源独立的单调计数器，主循环通过极短临界区获取差值，而不是多个 ISR
对同一个 event word 做不安全的读改写。

| 来源 | ISR 只做 | 主循环做 |
|---|---|---|
| ADC DMA TC | 清标志、读取 CT、记录完成 buffer index、递增序号 | 拷贝/封装 96 点、CRC、入页队列 |
| SPI DMA RX/TX | 清 TC/TE/DME/FE、记录结果 | 等 BSY 清零、CS 拉高、回调、重试/恢复 |
| USART1 RXNE | 字节入小环形缓冲 | 命令解析和输出 |
| SysTick | 递增毫秒 tick | deadline 判断、LED、统计窗口 |

ADC 完成事件不能只用一位 flag，否则主循环延迟时可能合并两个完成事件。实现时采用包含
`buffer_index + dma_sequence + timestamp` 的 4 项小描述符环，环满即记录硬错误和丢块。

## 4. 数据缓冲与所有权

### 4.1 ADC 双缓冲

- DMA M0/M1：各 96 个 `uint16_t`，由 DMA 循环交替写入。
- `CT` 指示当前 DMA 目标，刚完成的是另一块。
- 主循环必须在下一次同一内存目标被 DMA 重用前消费该块，期限为 9.6 ms。
- 若序号表明一个 buffer 被覆盖，递增 `adc_overrun_count`，不能静默继续。

### 4.2 记录页池

静态分配 8 个 256 字节 slot。READY 环只保存 slot index，不复制 256 字节 payload。

```text
FREE -> FILLING -> READY -> WRITING -> FREE
          |          |         |
          |          |         +-- SPI/NOR callback
          |          +------------ Recorder dequeue
          +----------------------- block packer owns slot
```

非法跳转、重复释放和非 owner 写入都计入 `ownership_error_count`。池满时不覆盖旧页：停止
采样或明确丢弃新块，并记录 overflow 和首个故障时间戳。默认策略是进入 DRAINING 后停止。

### 4.3 为什么仍允许一次 RAM 拷贝

从 192 字节 ADC buffer 拷贝到 256 字节 page slot，换来三个收益：DMA 可立即复用采样区；
页头和 CRC 有稳定快照；Flash 延迟由 8 页池吸收。10 kS/s 下拷贝速率仅 20 kB/s，
必须用 DWT 实测其 CPU 成本，而不是主观宣称“零开销”。

## 5. 状态机

### 5.1 顶层 Recorder（P0 简化版）

```text
IDLE -> PREPARING -> RECORDING -> DRAINING -> IDLE
  |         |            |           |
  +---------+------------+-----------+-> FAULT -> IDLE(after explicit reset)

BOOT -> RECOVERING -> IDLE
```

- `PREPARING`：检查固定数据区容量并预擦除本次记录窗口；P0 不写 A/B metadata。
- `RECORDING`：接收采样块、封装页、提交异步写入。
- `DRAINING`：停止 TIM2 触发，等待 DMA 停稳并写完 READY 页。
- `FAULT`：保存第一个错误，停止新生产，释放 CS 并终止当前事务；由明确命令复位状态。

自检由启动流程执行，不单独增加状态；`STOPPED` 与 `IDLE` 合并。这样保留异常闭环，同时
减少第一周需要覆盖的状态和迁移组合。

### 5.2 SPI Bus

```text
IDLE -> ASSERT_CS -> CONFIGURE_DMA -> ACTIVE
                                    | TC/ERR/timeout
                                    v
            COMPLETE <- DEASSERT_CS <- WAIT_SPI_NOT_BUSY
                |
                +-> callback -> IDLE

Any active state -> ABORT_DMA -> CLEAR_SPI -> DEASSERT_CS -> FAILED
```

P0 事务描述符包含 TX/RX 指针、长度、deadline、callback 和 callback context；SPI mode 和
最大时钟先作为总线静态配置，等多设备调度进入 P2 后再下沉到每笔事务。
CS 必须由 GPIO `BSRR` 原子控制；DMA TC 不代表最后一位已经离开 SPI，必须继续检查 BSY，
但该检查也受 deadline 限制。

### 5.3 NOR Flash

```text
UNINIT -> READ_ID -> IDLE
IDLE -> WREN -> ISSUE_COMMAND -> WAIT_WIP_CLEAR -> VERIFY/COMPLETE -> IDLE
                                  |
                                  +-> timeout -> RECOVER_SPI -> READ_ID -> IDLE/FAILED
```

页写入分两次操作：先写 offset 0..254，再单独把 offset 255 的 commit
从 `0xFF` 写为 `0x00`。中途掉电留下的页不会同时满足 commit 和 CRC。

P0 失败策略是记录首错并安全停止，不实现多级自动重试。SPI 清理、CS 拉高并返回可再次
初始化状态已经构成恢复闭环；复杂退避和 Flash 重新探测属于 P1/P2。

## 6. 接口草案

这些签名用于约束边界，实际头文件在板卡验收后创建：

```c
typedef enum {
    TB_OK = 0,
    TB_EINVAL = -1,
    TB_EBUSY = -2,
    TB_ETIMEOUT = -3,
    TB_EIO = -4,
    TB_EOVERFLOW = -5,
    TB_ECRC = -6,
    TB_ESTATE = -7,
    TB_ERANGE = -8
} Tb_Status;

typedef void (*SpiBus_Callback)(void *context, Tb_Status result,
                                uint32_t transferred);

Tb_Status SpiBus_Submit(const SpiBus_Transaction *transaction);
void SpiBus_Run(uint32_t now_ms);

Tb_Status Nor_PageProgramAsync(uint32_t address, const uint8_t *page,
                              Nor_Callback callback, void *context);
Tb_Status Nor_ReadAsync(uint32_t address, uint8_t *data, uint32_t length,
                       Nor_Callback callback, void *context);

Tb_Status PagePool_Acquire(uint8_t *slot_index, uint8_t **page);
Tb_Status PagePool_Publish(uint8_t slot_index);
Tb_Status PagePool_TakeReady(uint8_t *slot_index, const uint8_t **page);
```

边界检查至少覆盖空指针、零长度、page crossing、4 KiB erase alignment、Flash 容量上溢、
DMA `NDTR` 范围、slot index 和当前状态。

## 7. 寄存器级与 HAL 边界

| 模块 | V1 实现方式 | 重点寄存器/行为 |
|---|---|---|
| Clock tree | CubeMX/HAL 可生成 | HSI bring-up 后切 HSE 8 MHz / 168 MHz |
| USART1 | HAL 或简洁 LL | 只用于控制与导出，不是核心数据路径 |
| TIM2 | CMSIS 寄存器 | PSC、ARR、CR2.MMS=Update TRGO |
| ADC1 | CMSIS 寄存器 | SQR、SMPR、CR2 外部触发、DMA/DDS |
| DMA2 ADC | CMSIS 寄存器 | S4CR、NDTR、PAR、M0AR、M1AR、DBM、CT |
| SPI1 | CMSIS 寄存器 | CR1、CR2、SR、DR、OVR/BSY 处理 |
| DMA2 SPI | CMSIS 寄存器 | Stream2 RX、Stream3 TX、Channel 3 |
| Flash CS | CMSIS 寄存器 | GPIOB BSRR，默认高电平 |
| CRC | CMSIS 寄存器 | CRC DR/CR，单 page CRC，固定算法与字节序 |
| Measurement | CMSIS/CoreDebug | DWT CYCCNT、可选 GPIO scope pin |

寄存器写入顺序、关 Stream 后等待 EN 清零、清 DMA flag、SPI OVR 清除顺序等，必须在驱动
实现旁引用 RM0090 章节，并由逻辑分析仪和故障测试验证。

## 8. 可观测性

`TraceBox_Metrics` 至少包含：

- ADC block produced/consumed/overrun；
- page pool current/high-water/overflow；
- SPI transactions/bytes/TC/TE/DME/FE/timeout/recovery；
- NOR page program/erase/WIP timeout/verify failure；
- committed/recovered/rejected/CRC-failed pages；
- ISR max cycles、packer max cycles、main-loop max latency、CPU occupied cycles；
- first fault code/time/state 和 reset cause。

第一周只要求 max/total 等可低成本累计值；p99 分布、精确 CPU idle ratio 和长期统计直方图
属于 P1。P0 的 CPU occupied cycles 定义为 Polling 忙等 cycles，对比 DMA 的 submit、状态机
service 和 ISR cycles 之和，测试报告必须写清口径。

## 9. PC 与 Localhost 边界

固件只实现六条稳定命令和定长二进制 dump，不感知 HTTP。Python CLI 是串口协议、page codec、
CRC/sequence 校验和 CSV/report 生成的唯一实现；Node localhost server 只负责静态页面、参数
白名单、单操作协调和安全文件下载。命令行与网页必须调用同一 Python 代码，不能维护两套解析器。

详细 API 和进程模型见 `07_localhost_control.md`。该控制台属于 P0，但实时波形、WebSocket、
数据库、账号和云端部署均不属于 P0。
