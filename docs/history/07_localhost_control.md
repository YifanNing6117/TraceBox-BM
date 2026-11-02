> 历史设计记录：原开发目录中的路径、阶段目标与实验记录按当时状态保留。当前构建、运行和验证入口见[仓库 README](../../README.md)。部分原始日志与规划资料未纳入公开快照。

# Localhost 上位机控制台

## 1. P0 定位与实现状态

TraceBox-BM 第一周必须提供一个浏览器上位机，通过 `http://127.0.0.1:8765` 控制串口设备、
查看状态并下载验证结果。它是调试入口和最终 Demo 的一部分，不是独立的前端项目。

P0 不使用 React/Vue、数据库、账号系统、WebSocket 或云服务。页面由原生 HTML/CSS/JS
实现；Node.js 仅使用内置模块；串口和二进制记录解析由 Python 工具负责。这样可以复用
Mini-DCM 本地控制台已经验证过的进程边界，同时不复制 UDS 逻辑。

2026-09-14 已按本设计完成实现、无硬件 API 测试和 COM9 实机 HTTP 闭环。localhost
operation endpoint 已执行 `info` 与 3 秒 `full`，并通过 artifact route 下载 79,872 B dump；
下载哈希与本地文件一致。鼠标点击的视觉冒烟可在演示前补做，但没有独立串口或 codec 链路
仍处于未测状态。

## 2. 进程结构

```text
Browser (127.0.0.1:8765)
          |
          | HTTP JSON / file download
          v
Node local server (single-operation coordinator)
          |
          | execFile/spawn, no shell interpolation
          v
Python tracebox_cli.py + pyserial
          |
          | USART1 115200 control / 460800 binary dump
          v
STM32F407 TraceBox firmware
```

Node 不自行解析串口协议。Python CLI 是串口行为和 record codec 的唯一实现，命令行测试与
网页调用使用同一套代码，防止出现“两套上位机结果不一致”。

## 3. 页面最小功能

页面只包含四个区域：

1. **连接**：扫描 COM、选择端口和波特率、读取 `info`；
2. **控制**：`info`、`prepare`、`record`、`metrics`、`dump`、`bench` 六条固件命令的按钮和参数；
3. **观测**：当前 recorder/NOR/SPI 状态、produced/committed/dropped、pool depth/HWM、
   first fault 和最近日志；
4. **结果**：Polling/DMA 表格、dump 页数、CRC/sequence 结论、CSV/Markdown 下载链接。

P0 不画实时波形。浏览器能下载 CSV 后即完成闭环；波形和更丰富 dashboard 属于 P1。

## 4. 后台单操作模型

串口只能有一个 owner。Node server 维护一个 `active_operation`：

- `POST /api/operations` 校验白名单参数后启动一个 Python 子进程，立即返回 operation id；
- `GET /api/operations/current` 返回 `queued/running/succeeded/failed`、开始时间、日志尾部和
  结果文件；
- 有 operation 运行时，新的串口操作返回 HTTP 409，不允许两个进程竞争 COM；
- 子进程异常退出、超时或 server 结束时，状态必须变为 failed，不能永久显示 running；
- 前端每 500 ms~1 s 轮询状态，不引入 WebSocket。

`record` 固件命令立即返回，MCU 自主采集；网页通过短 `metrics` 操作查看进度。`prepare`、
`dump`、`bench` 允许作为后台长操作。二进制 dump 先保存到 `artifacts/raw/`，校验成功后再
生成 `artifacts/exports/` 下的 CSV/Markdown。

## 5. P0 API

| Method | Route | 用途 |
|---|---|---|
| GET | `/api/health` | server 和 Python 环境状态 |
| GET | `/api/serial/ports` | 枚举串口 |
| POST | `/api/operations` | 启动白名单操作 |
| GET | `/api/operations/current` | 查询当前/最近操作 |
| GET | `/api/artifacts` | 列出允许下载的结果 |
| GET | `/api/artifacts/<name>` | 下载安全目录内文件 |

允许的 operation 只有 `info/prepare/record/recover/metrics/dump/full/fault-torn/bench`。
端口名、波特率、记录秒数等参数必须做类型和范围检查，不能把浏览器字符串拼接到 shell
命令。

## 6. 安全和边界

- server 只监听 `127.0.0.1`，不监听 `0.0.0.0`；
- 使用 `execFile`/`spawn` 的参数数组，不使用 `exec` 或 shell command string；
- 下载路径经 `path.resolve` 后必须仍位于项目 `artifacts/`；
- API request body 设置大小上限；
- 同一时刻只允许一个串口操作；
- UI 不提供任意命令执行框，只有固定 operation 白名单；
- 页面上明确显示固件 Git commit、Flash JEDEC ID 和本次结果时间戳。

## 7. P0 验收

- 一条命令启动 server，并打印 `http://127.0.0.1:8765`；
- Chrome/Edge 能扫描端口并运行 `info`；
- 页面能完成 `prepare -> record -> metrics -> dump -> validate`；
- 长操作期间页面仍能显示 operation 状态，重复操作收到明确的 busy 提示；
- 页面展示 60 s 样本数、CRC、sequence、dropped、HWM 和 Polling/DMA 指标；
- CSV、Markdown 报告可从页面下载；
- server 重启后不会把旧 running 状态误报成仍在执行。

## 8. 实际目录

```text
web-console/
├── start.ps1
├── package.json
├── server/src/server.js
├── test/server.test.js
└── frontend/
    ├── index.html
    └── src/{app.js,styles.css}
```

页面视觉可以参考 Mini-DCM Workbench，但所有协议文案、接口和结果字段必须针对 TraceBox
重新定义，禁止保留 UDS/CAN/DID 等无关概念。
