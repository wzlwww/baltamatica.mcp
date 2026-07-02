# baltamatica.mcp

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![MCP Protocol](https://img.shields.io/badge/MCP-Compatible-green.svg)](https://modelcontextprotocol.io)
[![Baltamatica](https://img.shields.io/badge/Baltamatica-2025-orange.svg)](https://www.baltamatica.com)

> 🔗 让 AI 代理（Claude Code / Cursor / Codex）直接驱动国产科学计算软件 **北太天元（Baltamatica）** 进行交互式数值计算。

---

## 📖 项目简介

**baltamatica.mcp** 是一个基于 [Model Context Protocol (MCP)](https://modelcontextprotocol.io) 的开源服务，为大语言模型提供与北太天元科学计算内核之间的双向通信桥梁。

通过本项目，AI 代理可以：

- 🧮 **执行代码**：直接在北太天元中运行 `.m` 脚本或单行表达式，并取回控制台输出
- 📊 **读取变量**：获取工作区中矩阵、向量、结构体、元胞等变量的值（BEX 后端二进制全保真）
- ✍️ **写入变量**：把标量/向量/矩阵注入工作区（`set_variable`）
- 🔍 **查询状态**：列出当前工作区中所有变量的名称、类型和维度
- 📂 **管理脚本**：运行本地 `.m` 脚本文件
- 🧹 **清空工作区**：重置计算环境

**当前状态**：两套后端均可用。

- **CLI 后端**：通过北太天元命令行入口执行代码，用 `.mat` 状态文件在多次 MCP 调用之间保持工作区变量。支持全部 6 个工具。
- **BEX 后端**：在北太天元 GUI 进程内运行的 C 桥接（JSON-over-TCP），支持 `execute_code`、`run_script`、`list_variables`、`get_variable`、`set_variable`、`clear_workspace`：
  - `get_variable` 对数值/逻辑数组（实数**和复数**、任意大小）走 base64 二进制全保真回传，对字符/字符串/结构体/元胞走结构化 JSON；
  - `set_variable` 用 `bxAddVariable` 把标量/向量/矩阵注入工作区；
  - `execute_code` / `run_script` 用 `evalc` 捕获控制台输出并解析 `BALTAMATICA_ARTIFACT=` 文件产物；
  - 桥接生命周期健壮（可靠 `stop`、Ctrl-C 恢复、`background` 模式），并可触发 GUI Figure 弹窗（但北太天元本身无图形导出函数，见下）。

---

## 🏗️ 系统架构

项目提供两套后端。**CLI 后端**通过北太天元命令行入口执行代码，用 `.mat` 状态文件保持工作区变量，无需编译，适合快速上手。**BEX 后端**是一个在北太天元 GUI 进程内运行的 C 桥接，走 **JSON-over-TCP**，提供进程内低延迟长连接、二进制变量传输和变量注入。两者暴露相同的 MCP 工具集。

```
┌──────────────────────────────────────────────────────────┐
│            AI Agent (Claude Code / Cursor / Codex)         │
└─────────────────────────┬────────────────────────────────┘
                          │ MCP Protocol (stdio)
                          ▼
┌──────────────────────────────────────────────────────────┐
│               Python MCP Server (baltamatica-mcp)          │
│  execute_code / run_script / list_variables /              │
│  get_variable / set_variable / clear_workspace             │
│                     Engine Dispatcher                      │
└───────────────┬──────────────────────────┬───────────────┘
                │ (--backend cli)           │ (--backend bex)
                ▼                           ▼
     ┌────────────────────┐      ┌──────────────────────────┐
     │    CLI Backend     │      │       BEX Backend        │
     │ subprocess + .mat  │      │  JSON-over-TCP client    │
     └─────────┬──────────┘      └────────────┬─────────────┘
               ▼                               ▼
   baltamatica -nodesktop -s "…"    mcp_bridge (BEX in GUI, TCP 31415)
```

### 后端状态

| 特性 | CLI 后端 | BEX 后端 |
|:---|:---:|:---:|
| `execute_code` | ✅ | ✅ `evalc` 捕获输出 |
| `run_script` | ✅ | ✅ `evalc` 捕获输出 |
| `list_variables` | ✅ `whos` 解析 | ✅ SDK 变量枚举 |
| `get_variable` | ✅ `disp()` 文本 | ✅ 二进制全保真 + 结构化 JSON |
| `set_variable` | ✅ 字面量代码 | ✅ `bxAddVariable`（float64/bool） |
| `clear_workspace` | ✅ | ✅ `clear` |
| 工作区状态保持 | ✅ `.mat` 状态文件 | ✅ BEX 进程长连接 |
| 文件产物反馈 | ✅ artifact marker | ✅ 从捕获输出解析 marker |

---

## 🚀 快速开始

### 前置条件

- Python 3.10+
- [北太天元 2025](https://www.baltamatica.com/download.html)（社区版即可）
- 北太天元命令行入口（macOS 通常是 `/Applications/Baltamatica.app/Contents/MacOS/baltamatica`）
- C 编译器（仅使用 BEX 后端时需要；北太天元自带 `bex` 编译器会调用系统 clang/gcc）

### 安装

```bash
git clone https://github.com/wzlwww/baltamatica.mcp.git
cd baltamatica.mcp
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
```

### BEX 插件状态

BEX JSON 协议见 [docs/bex-protocol.md](docs/bex-protocol.md)，桥接使用与生命周期见
[docs/bex-plugin.md](docs/bex-plugin.md) 与 [docs/bex-bridge.md](docs/bex-bridge.md)。
当前 BEX 桥接已实现 `execute_code`、`run_script`、`clear_workspace`、`list_variables`
和 `get_variable`，返回结构化成功/错误结果。

**编译并加载**（在北太天元 GUI 命令行）：

```matlab
clear mcp_bridge
cd '/path/to/baltamatica.mcp/bex'
bex 'mcp_bridge.c'                                   % 生成 mcp_bridge.bexmaci64 / .bexa64 / .bexw64
addpath('/path/to/baltamatica.mcp/bex'); savepath   % 让 mcp_bridge 常驻搜索路径
```

**两种运行模式**：

```matlab
mcp_bridge()              % 前台：阻塞命令行，直到被停止
mcp_bridge('background')  % 后台：立即返回，命令行空着（推荐，可从同一 GUI 控制）
```

启动后让 Python MCP server 连接同一端口：

```bash
python -m baltamatica_mcp --backend bex --bex-host 127.0.0.1 --bex-port 31415
```

**停止**：

```matlab
mcp_bridge('stop')        % 可靠停止；前台被 Ctrl-C 打断后也能释放端口
```

桥接把监听 socket 记在进程全局态里：`stop` 能在前台被 Ctrl-C 打断后直接关闭 socket 释放端口，
重跑 `mcp_bridge()` 会自动回收泄漏的 socket 而不是 bind 失败。若 `mcp_bridge` 不在搜索路径上
（例如重启后 `addpath` 丢失），可用纯 TCP 的兜底工具停止，完全不依赖路径：

```bash
PYTHONPATH=src python -m baltamatica_mcp.bex_shutdown
```

**已知限制**：

- `get_variable`：数值/逻辑数组（实数**和复数**、任意大小）走 base64 二进制全保真回传；字符/字符串/结构体/元胞走结构化 JSON。极大的结构体/元胞会按上限截断（见 `truncated` 字段）；结构体/元胞里嵌套的数值目前是列主序扁平数组。
- `set_variable`：目前支持 float64 和 bool、二维以内；受请求行大小限制（约 4500 个 double），暂不支持整数/复数类型和超大数据的流式接收。
- 屏幕绘图可用（`figure`/`plot` 等会在 GUI 弹窗），但**图形导出到文件不可用**——遍查北太天元全部 3736 个文档函数，均无 `saveas`/`print`/`exportgraphics`/`getframe`/`imwrite` 等图形导出函数（这是北太天元本身的能力缺失）。替代方案：用 `get_variable` 取回绘图数据由 AI 端渲染，或用 `writematrix`/`writetable`/`save` 把数据导出成文件 + `BALTAMATICA_ARTIFACT=` 标记回传（见下）。
- CLI 后端无需编译、跨平台，适合快速上手；BEX 后端功能更全（二进制传输、变量注入、输出捕获）。

**文件产物反馈(BEX 也支持)**:`execute_code` / `run_script` 现在捕获控制台输出,脚本用
`fprintf('BALTAMATICA_ARTIFACT=text/csv:/tmp/x.csv\n')` 声明的文件会被解析进返回的
`artifacts` 列表(路径、MIME、是否存在、大小)。

### 在 Claude Desktop 中配置

编辑 `~/Library/Application Support/Claude/claude_desktop_config.json`：

```json
{
  "mcpServers": {
    "baltamatica": {
      "command": "/path/to/baltamatica.mcp/.venv/bin/python",
      "args": ["-m", "baltamatica_mcp"]
    }
  }
}
```

### 在 Claude Code 中配置

```bash
claude mcp add baltamatica -- /path/to/baltamatica.mcp/.venv/bin/python -m baltamatica_mcp
```

### CLI Fallback 模式（无需编译）

如果暂时没有 C 编译环境，可以使用 CLI 后端启动：

```bash
BALTAMATICA_CLI=/path/to/baltamatica python -m baltamatica_mcp --backend cli
```

macOS 社区版通常使用真实可执行文件，而不是 `baltamaticaC.sh` 包装脚本：

```bash
/Applications/Baltamatica.app/Contents/MacOS/baltamatica
```

也可以直接通过参数指定北太天元命令行入口和单次执行超时：

```bash
python -m baltamatica_mcp --backend cli --cli-executable /path/to/baltamatica --timeout 30
```

CLI 后端会用 `.mat` 状态文件在多次 MCP 调用之间保存工作区变量。默认使用临时状态文件，
也可以显式指定：

```bash
python -m baltamatica_mcp --backend cli --state-file /tmp/baltamatica-mcp-state.mat
```

### 在 Codex 中配置

```bash
codex mcp add baltamatica \
  --env PYTHONPATH=/path/to/baltamatica.mcp/src \
  --env BALTAMATICA_CLI=/Applications/Baltamatica.app/Contents/MacOS/baltamatica \
  -- python3 -m baltamatica_mcp --backend cli --timeout 30
```

配置后可通过 MCP 工具调用 `execute_code`、`run_script`、`list_variables`、
`get_variable`、`set_variable` 和 `clear_workspace`。

### 开发测试

运行不依赖北太天元安装的单元测试：

```bash
PYTHONPATH=src pytest -q -m "not integration"
PYTHONPATH=src python3 -m compileall -q src tests
```

如果本机已安装北太天元 CLI，可以启用真实集成测试：

```bash
BALTAMATICA_CLI=/Applications/Baltamatica.app/Contents/MacOS/baltamatica \
  PYTHONPATH=src pytest -q -m integration
```

---

## 📁 项目结构

```
baltamatica.mcp/
├── README.md
├── LICENSE
├── pyproject.toml
├── .github/workflows/ci.yml
├── src/baltamatica_mcp/
│   ├── __init__.py
│   ├── __main__.py
│   ├── server.py           # MCP Server & Tool 注册
│   ├── engine.py           # 后端协议与结果类型
│   ├── backend_cli.py      # CLI 后端：subprocess + .mat 状态文件
│   ├── backend_bex.py      # BEX JSON-over-TCP 客户端
│   ├── serializer.py       # 变量二进制解码/编码与结构化呈现
│   └── bex_shutdown.py     # 纯 TCP 关闭 BEX 桥接的兜底工具
├── bex/
│   ├── CMakeLists.txt      # BEX CMake 构建配置
│   ├── mcp_protocol.h      # BEX JSON 协议常量
│   ├── mcp_bridge.c        # BEX TCP 桥接源码（主体）
│   └── bex_plot_probe.c    # 绘图能力探针
├── tests/
│   ├── test_backend_cli.py
│   ├── test_backend_bex.py
│   ├── test_serializer.py
│   ├── test_bex_sources.py
│   ├── test_bex_shutdown.py
│   ├── test_server.py
│   ├── test_integration_cli.py   # 标记 integration
│   ├── test_integration_bex.py   # 标记 integration
│   └── fixtures/sample_script.m
├── docs/
│   ├── contributing.md
│   ├── bex-protocol.md
│   ├── bex-plugin.md
│   ├── bex-bridge.md
│   ├── bex-plot-probe.md
│   └── pr-plan.md
└── examples/
    ├── monte_carlo_pi.m
    ├── numerical_pipeline_demo.m
    ├── artifact_export_demo.m
    └── bex_plot_probe_demo.m
```

---

## 🛠️ MCP Tools（暴露给 AI 的工具接口）

| Tool 名称 | 参数 | 描述 | CLI 后端 | BEX 后端 |
|:---|:---|:---|:---:|:---:|
| `execute_code` | `code: string` | 执行代码并返回控制台输出 | ✅ | ✅ `evalc` 捕获输出 |
| `run_script` | `file_path: string` | 运行 `.m` 脚本文件 | ✅ | ✅ `evalc` 捕获输出 |
| `list_variables` | — | 列出工作区所有变量（名称、类型、维度） | ✅ `whos` 解析 | ✅ SDK 变量枚举 |
| `get_variable` | `name: string` | 获取变量值 | ✅ `disp()` 文本 | ✅ 二进制全保真 + 结构化 JSON |
| `set_variable` | `name: string, data` | 创建/覆盖工作区变量 | ✅ 字面量代码 | ✅ `bxAddVariable`（float64/bool） |
| `clear_workspace` | — | 清空工作区状态 | ✅ | ✅ `clear` |

### 文件产物反馈

脚本可以通过标准输出声明生成的文件产物：

```matlab
fprintf('BALTAMATICA_ARTIFACT=text/csv:/tmp/result.csv\n');
```

MCP 返回值会包含 `artifacts` 列表，记录文件路径、MIME 类型、是否存在和文件大小。若省略 MIME 类型：

```matlab
fprintf('BALTAMATICA_ARTIFACT=/tmp/plot.png\n');
```

服务会根据扩展名推断常见类型，例如 `image/png`、`application/pdf`、`text/csv`。示例见
`examples/artifact_export_demo.m`。

---

## 🗺️ 开发路线图 (Roadmap)

详细 PR 拆分和实现方案见 [docs/pr-plan.md](docs/pr-plan.md)。

### 已完成

- [x] MCP Server 骨架（`FastMCP`）
- [x] CLI 后端（`subprocess` + `baltamatica -nodesktop -s`）
- [x] `execute_code` / `run_script`
- [x] `.mat` 状态文件保持 CLI 工作区
- [x] `list_variables` / `get_variable` / `clear_workspace`
- [x] 可选真实集成测试与 GitHub Actions CI
- [x] 数值计算示例：`examples/numerical_pipeline_demo.m`
- [x] 文件产物反馈：`BALTAMATICA_ARTIFACT=...`
- [x] BEX JSON 协议设计与 Python TCP 客户端骨架
- [x] BEX 插件最小可用版
- [x] BEX `list_variables` / `get_variable` 文本变量读取
- [x] BEX 小型实数数值/逻辑数组结构化 JSON 读取
- [x] BEX 生命周期健壮化：可靠 `stop` / Ctrl-C 恢复 / 自愈重绑 / `background` 模式 / 状态返回值
- [x] BEX `get_variable` 存在性预检查（避免不存在变量在 GUI 里回显 `evalin` 错误）
- [x] BEX 数值/逻辑二进制全保真传输（含复数）+ 字符/字符串/结构体/元胞结构化序列化
- [x] BEX `set_variable`：从标量/向量/矩阵注入工作区变量（`bxAddVariable`）
- [x] BEX `execute_code` / `run_script` 控制台输出捕获（`evalc` + `try/catch`，成功返回 stdout、失败返回错误信息）
- [x] BEX 文件产物反馈：从捕获输出解析 `BALTAMATICA_ARTIFACT=` 标记
- [x] 绘图导出调研：确认北太天元无任何图形导出函数（3736 个函数全查），改走数据侧回传
- [x] `background` 模式线程安全评估：桥接串行化所有请求（800 顺序 + 并发双连接零错误）；唯一风险是后台模式下手动同时操作 GUI（见 [docs/bex-bridge.md](docs/bex-bridge.md) 线程与并发一节）
- [x] 自动化 BEX 集成测试（`bex` 编译校验 + 运行中桥接往返，标记 `integration`）
- [x] 发布体验：`baltamatica-mcp` 控制台入口、项目 URL、`set_variable` 工具与文档

### 下一步

- [ ] BEX `set_variable` 扩展：整数/复数类型、大数据流式接收（当前 float64/bool、受请求行大小限制）
- [ ] BEX 图形导出到文件：受限于北太天元本身缺少 `saveas`/`print`/`exportgraphics` 等函数，需厂商侧支持
- [ ] 打包发布到 PyPI，提供预编译的 BEX 二进制

---

## 🔧 核心依赖的北太天元 SDK API

本项目依赖北太天元 BEX SDK (`bex.h`, API v3.9) 提供的以下关键接口：

| API 函数 | 功能 |
|:---|:---|
| `bxEvalString(expr)` | 执行一条字符串命令 |
| `bxEvalIn(ws, expr, plhs)` | 在指定工作区执行表达式并捕获返回值 |
| `bxCallBaltamatica(...)` | 通过函数名调用北太天元内置/自定义函数 |
| `bxGetVariableNames(result, num)` | 获取当前工作区所有变量名 |
| `bxAddVariable(name, value, mode)` | 向工作区注入变量 |
| `bxRemoveVariable(name)` | 删除工作区变量 |
| `bxGetDoublesRO/bxGetComplexDoublesRO/...(ba)` | 只读获取各类型矩阵数据指针（二进制回传） |
| `bxCreateDoubleMatrix/bxCreateLogicalMatrix(...)` | 创建矩阵（`set_variable` 注入） |
| `bxGetDoublesRW/bxGetLogicalsRW(ba)` | 读写数据指针（写入注入的数据） |
| `bxGetString / bxGetFieldByNumberRO / bxGetCellRO` | 读取字符串/结构体字段/元胞元素（结构化序列化） |
| `bxGetDimensions(ba)` | 获取数组维度信息 |
| `bxGetClassID(ba)` | 获取变量的数据类型 |
| `bxArrayToCStr(ba, ...)` / `bxAsCStr(ba, ...)` | 将变量格式化/转换为字符串 |
| `bxPrintf(...)` | 向命令行窗口输出（桥接状态信息） |

`execute_code` / `run_script` 通过 `bxCallBaltamatica` 调用内置的 `evalc` 函数捕获控制台输出。

---

## 🤝 贡献指南

欢迎贡献代码！请参阅 [CONTRIBUTING.md](docs/contributing.md)。

### 分支模型

- `main`：稳定发布分支，始终保持可用
- `develop`：开发主分支，所有 feature 合入此处
- `feature/<name>`：功能分支，从 `develop` 拉出
- `fix/<name>`：Bug 修复分支

### 提交规范

使用 [Conventional Commits](https://www.conventionalcommits.org/) 规范：

```
feat: 添加 execute_code 工具实现
fix: 修复控制台输出截断问题
docs: 更新 README 安装说明
refactor: 重构引擎通信模块
test: 添加变量序列化单元测试
chore: 更新 CI 配置
```

---

## 📄 License

MIT License © 2026 [wzlwww](https://github.com/wzlwww)
