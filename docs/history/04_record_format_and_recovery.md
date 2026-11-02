> 历史设计记录：原开发目录中的路径、阶段目标与实验记录按当时状态保留。当前构建、运行和验证入口见[仓库 README](../../README.md)。部分原始日志与规划资料未纳入公开快照。

# Flash 记录格式与掉电恢复

## 1. 设计原则

- V1 使用线性、预擦除记录窗口；写满即停止，不在记录过程中回卷覆盖。
- 一个 ADC DMA 块对应一个 256 字节物理 Flash 页。
- 页面先写内容，再单独写最后一个提交字节。
- 只有 magic、版本、长度、commit 和 CRC 全部通过的页才是有效页。
- 序列化采用小端；CRC 算法和 padding 明确定义，PC 与 MCU 使用同一测试向量。

## 2. 256 字节采样页

| Offset | Size | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `TBP1` |
| 4 | 1 | `format_version` | V1 = 1 |
| 5 | 1 | `record_type` | 1 = ADC sample block |
| 6 | 1 | `header_size` | 32 |
| 7 | 1 | `flags` | trigger/fault/session flags |
| 8 | 4 | `page_sequence` | session 内单调递增 |
| 12 | 4 | `t0_us` | 第一采样点时间戳，32 bit wrap 允许 |
| 16 | 4 | `sample_period_ns` | V1 = 100000 |
| 20 | 2 | `sample_count` | V1 = 96 |
| 22 | 2 | `payload_length` | V1 = 192 |
| 24 | 4 | `page_crc32` | header + payload 的单一 CRC |
| 28 | 4 | `capture_id` | P0 由启动命令指定，默认 0 |
| 32 | 192 | `samples` | 96 x little-endian uint16 |
| 224 | 31 | `reserved` | 保持 0xFF，供兼容扩展 |
| 255 | 1 | `commit` | 擦除态 0xFF；完整提交后写 0x00 |

所有结构按字节显式编码，禁止直接把未 `packed` 的 C 结构体强转写入 Flash。即便使用 packed
结构体，也必须由静态断言验证 offset 和 size，并以编码器输出为协议真值。

## 3. CRC 规则

V1 使用 STM32F407 硬件 CRC 单元天然支持的 CRC-32/MPEG-2：

- polynomial `0x04C11DB7`；
- init `0xFFFFFFFF`；
- refin=false，refout=false，xorout=`0x00000000`；
- 计算前将 `page_crc32` 字段置 0；
- 覆盖 offset 0..223，即完整 header 和有效 samples，不覆盖保留区与 commit；
- 输入按序列化后的字节流组成大端 32-bit word 写入 CRC DR；本页长度天然为 4 字节整数倍。

实现前必须建立至少三个 MCU/PC 公共测试向量：空载荷、`123456789`、跨 32-bit 边界长度。
若实测发现 HAL/编译器字节装载方式造成歧义，以显式 byte-to-word 组装函数修正，不能修改
PC 工具“适配错误结果”。

## 4. 写入提交协议

对目标页地址 `P`：

1. 检查 `P` 为 256 B 对齐，且 `P + 255` 在 session 窗口内。
2. 检查目标页处于擦除态；Debug/验证模式至少抽样读取。
3. 在 RAM slot 构造 header、payload 和单一 page CRC，commit 保持 `0xFF`。
4. `WREN`，Page Program `P + [0..254]`，异步轮询 WIP，受 program deadline 限制。
5. `WREN`，单独 program `P + 255 = 0x00`。
6. WIP 清零后递增 `committed_pages`，释放 slot，地址推进 256 B。

任何一步失败都不尝试把该页“补成有效”。记录错误并停止/恢复；重新开始时换到下一个已擦除
页或重新 prepare session。

## 5. Flash 空间布局

以 W25Q64 8 MiB 为例：

| 区域 | 地址 | P0 用途 |
|---|---|---|
| Reserved | `0x000000..0x001FFF` | P0 不使用，保留给 P2 metadata A/B |
| Data | `0x002000..capacity-1` | 线性预擦除采样页 |

P0 将 `0x002000` 固定为 data base。`prepare` 根据 60 秒目标计算并擦除足够扇区，然后从
data base 开始写；下一次记录重新 prepare。这样无需先实现 metadata 一致性，仍能在重启后
从固定地址完成掉电恢复。A/B checkpoint、generation 和快速定位进入 P2。

## 6. 启动恢复算法

1. 从固定 `data_base = 0x002000` 按 256 B 扫描。
2. 对每页检查 magic、version、header/payload 长度、commit 和 page CRC。
3. 记录连续有效序号；遇到第一个 erased 或 invalid 页即停止。
4. `recovered_end` 指向最后有效页之后；invalid 尾页计入 `rejected_torn_pages`。
5. P0 恢复后只允许 dump，不原地续写残页所在扇区。继续采集必须重新 prepare 数据区。

这种策略把 V1 的掉电一致性与复杂循环回收解耦。目标是“绝不把不完整数据报告成有效”，
不是宣称一次掉电零数据损失。理论损失上界为正在填充/写入的少量尾块，必须用实测统计。

## 7. P0 UART 导出格式

为避免把 1.6 MB 数据编码成缓慢的 ASCII hex，`dump` 使用定长二进制：

1. MCU 输出一行 ASCII：`TB_DUMP_BEGIN <valid_pages> 256\r\n`；
2. PC 解析页数后，精确读取 `valid_pages * 256` 个 raw bytes；
3. MCU 输出一行 ASCII：`\r\nTB_DUMP_END\r\n`；
4. PC 按本页格式逐页检查 commit、CRC 和 sequence，再生成 CSV/Markdown。

因为长度由 preamble 给出，raw payload 不需要转义或复杂帧协议。串口中断或复位导致长度不足
时，PC 报 incomplete dump，不能把部分文件当作成功结果。板卡 bring-up 使用 115200 8N1，
正式 dump 目标 460800 8N1，实物不稳定时允许降到 230400 并记录实测。

## 8. 格式演进

- 未识别版本直接报 `TB_EFORMAT`，不能按 V1 猜测解析。
- V2 的 Mini-DCM TraceFrame 可使用新的 `record_type`，仍封装在相同 256 B page envelope 中。
- P1 可加入单区 checkpoint；P2 再加入 A/B metadata、generation 和快速恢复。
- V2 若加入循环覆盖，使用 sector epoch + erase-ahead，不改变 V1 已提交页的基本判定条件。
