# 开源快照验证

验证日期：2026-10-27。以下命令在本次整理后的独立仓库中运行。

| 检查 | 结果 |
|---|---|
| `python -m unittest discover -s tests -v` | 21 项通过；Python 3.12.14 |
| `node --test web-console/test/server.test.js` | 5 项通过，包含本机 HTTP 接口验证 |
| `python tools/offline_full_scale_test.py` | PASS；6,250 页、600,000 点、1,600,000 B 二进制数据 |
| `tools/build_keil.ps1 -KeilExe <Keil路径>` | Keil ARMCC 5.06 / build 528；0 错误、0 警告 |
| `python tools/audit_publication.py` | 142 个待发布文件，0 项发现；未纳入历史 Git 对象 |
| 硬件复测 | 本次未连接开发板或执行 Flash 擦写 |
| GitHub Actions | 工作流已提供，尚未上传运行 |

本轮 Keil 构建输出 HEX 的 SHA-256 为 `B1C37FF755419D543B890D42A9C576A3C9287814028CFC4140B42E8AD8A86445`。镜像由本地构建生成，已忽略，不纳入源码发布。

离线全规模测试使用合成数据，验证页格式、CRC、序号和 CSV 导出。历史文档中的采样、DMA 开销和掉电恢复数字属于此前的硬件实验；本次测试不替代这些硬件测量。

本轮首先在受限沙箱中运行测试，临时文件访问与回环连接受到环境限制；在本机正常权限下重跑后，上述测试均通过。Keil/ARMCC 是本机商业工具链，GitHub 工作流只运行无需 Keil 的主机检查。
