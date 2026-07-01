# baltamatica.mcp

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![MCP Protocol](https://img.shields.io/badge/MCP-Compatible-green.svg)](https://modelcontextprotocol.io)
[![Baltamatica](https://img.shields.io/badge/Baltamatica-2025-orange.svg)](https://www.baltamatica.com)

> 🔗 让 AI 代理（Claude Code / Cursor / Codex）直接驱动国产科学计算软件 **北太天元（Baltamatica）** 进行交互式数值计算。

---

## 📖 项目简介

**baltamatica.mcp** 是一个基于 [Model Context Protocol (MCP)](https://modelcontextprotocol.io) 的开源服务，为大语言模型提供与北太天元科学计算内核之间的双向通信桥梁。

通过本项目，AI 代理可以：

- 🧮 **执行代码**：直接在北太天元中运行 `.m` 脚本或单行表达式
- 📊 **读取变量**：获取工作区中矩阵、向量、结构体等变量的值
- 🔍 **查询状态**：列出当前工作区中所有变量的名称、类型和维度
- 📂 **管理脚本**：运行本地 `.m` 脚本文件
- 🧹 **清空工作区**：重置计算环境

**当前状态**：CLI 后端已经可用，支持执行代码、运行脚本、查询变量、读取变量和清空工作区。CLI 模式通过 `.mat` 状态文件在多次 MCP 调用之间保持工作区变量；BEX 后端已经包含 JSON 协议、Python TCP 客户端和实验性 BEX 桥接插件，可在 Baltamatica GUI 进程内执行代码、运行脚本、清空工作区，并触发 GUI Figure 弹窗。BEX 变量读取和结构化序列化仍在后续 PR 中完成。

---

## 🏗️ 系统架构

当前实现优先提供 **CLI fallback 后端**，通过北太天元命令行入口执行代码，并用 `.mat` 状态文件保持工作区变量。BEX 路径提供实验性的 **BEX 插件 + TCP Socket** 后端：插件加载到 Baltamatica GUI 进程后，会在本机回环地址启动 JSON-over-TCP 桥接服务，Python MCP 服务可以后台连接这个桥接服务并把 `plot(...)` 等命令送入 GUI 解释器。

```
┌──────────────────────────────────────────────────────────┐
│                AI Agent (Claude Code / Cursor / Codex)   │
└─────────────────────────┬────────────────────────────────┘
                          │ MCP Protocol (stdio)
                          ▼
┌──────────────────────────────────────────────────────────┐
│               Python MCP Server (baltamatica-mcp)        │
│                                                          │
│  execute_code / run_script / list_variables              │
│  get_variable / clear_workspace                          │
│                                                          │
│             Engine Dispatcher                            │
│           ┌────────┴────────┐                            │
│           ▼                 ▼                            │
│      CLI Backend       BEX Backend                       │
│ subprocess + .mat      TCP JSON Client                   │
│    state file                                             │
└───────────┬─────────────────┬────────────────────────────┘
            ▼                 ▼
  Baltamatica CLI      GUI-loaded BEX bridge
```

### 后端状态

| 特性 | CLI 后端（已实现） | BEX 后端（实验性桥接） |
|:---|:---:|:---:|
| `execute_code` | ✅ | ✅ `eval` via `bxCallBaltamatica` |
| `run_script` | ✅ | ✅ `run('...')` |
| `list_variables` | ✅ `whos` 解析 | ✅ SDK 变量名 + 元数据 |
| `get_variable` | ✅ `disp()` 文本 | ✅ `bxArrayToCStr` 文本 |
| `clear_workspace` | ✅ | ✅ `clear` |
| 工作区状态保持 | ✅ `.mat` 状态文件 | ✅ GUI 进程内保持 |
| GUI 画图弹窗 | ❌ headless CLI 边界 | ✅ GUI 进程加载后可弹 Figure |
| 图像/文件产物反馈 | ✅ 文件 artifact marker | 协议支持，插件侧待补齐 |

---

## 🚀 快速开始

### 前置条件

- Python 3.10+
- [北太天元 2025](https://www.baltamatica.com/download.html)（社区版即可）
- 北太天元命令行入口（macOS 通常是 `/Applications/Baltamatica.app/Contents/MacOS/baltamatica`）
- BEX 编译器（可选，仅 BEX GUI 桥接模式需要；macOS 通常是 `/Applications/Baltamatica.app/Contents/MacOS/bex`）

### 安装

```bash
git clone https://github.com/wzlwww/baltamatica.mcp.git
cd baltamatica.mcp
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
```

### BEX GUI 桥接模式（实验性）

BEX JSON 协议见 [docs/bex-protocol.md](docs/bex-protocol.md)，使用和排障见
[docs/bex-bridge.md](docs/bex-bridge.md)。当前仓库包含最小 BEX 桥接插件
`bex/mcp_bridge.c`，加载到 Baltamatica GUI 后会监听 `127.0.0.1:31415`。后续 MCP 调用可以在后台通过 TCP 进入 GUI 进程，因此 `plot(...)` 会弹出 Baltamatica Figure。当前桥接循环运行在 BEX 调用线程内，以保证解释器和 GUI 绘图调用发生在正确线程；代码执行通过 Baltamatica `eval` 完成。因此启动 `mcp_bridge()` 后，GUI 命令窗口会被桥接服务占用，直到桥接进程退出或收到调试用 `shutdown` 请求。

先在仓库根目录编译插件：

```bash
/Applications/Baltamatica.app/Contents/MacOS/bex bex/mcp_bridge.c
```

然后在 Baltamatica GUI 中把仓库根目录加入路径，并运行：

```matlab
mcp_bridge()
```

也可以指定端口：

```matlab
mcp_bridge(31416)
```

桥接启动后，在另一个终端启动 BEX 后端：

```bash
python -m baltamatica_mcp --backend bex --bex-host 127.0.0.1 --bex-port 31415
```

限制：当前 BEX 桥接插件已实现 `execute_code`、`run_script`、`clear_workspace`、`list_variables` 和 `get_variable`。变量值先以文本形式返回；大数组输出会截断，结构化 JSON/二进制矩阵传输计划在后续序列化 PR 中完成。

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
`get_variable` 和 `clear_workspace`。

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
│   └── serializer.py       # 后续变量序列化占位
├── bex/
│   ├── mcp_bridge.c        # 实验性 BEX JSON-over-TCP 桥接插件
│   └── bex_plot_probe.c    # BEX GUI 画图能力探针
├── tests/
│   ├── test_backend_cli.py
│   ├── test_backend_bex.py
│   ├── test_integration_cli.py
│   ├── test_server.py
│   └── fixtures/sample_script.m
├── docs/
│   ├── contributing.md
│   ├── bex-protocol.md
│   └── pr-plan.md
└── examples/
    ├── artifact_export_demo.m
    ├── bex_plot_probe_demo.m
    ├── monte_carlo_pi.m
    └── numerical_pipeline_demo.m
```

---

## 🛠️ MCP Tools（暴露给 AI 的工具接口）

| Tool 名称 | 参数 | 描述 | CLI 后端 | BEX 后端 |
|:---|:---|:---|:---:|:---:|
| `execute_code` | `code: string` | 执行代码并返回控制台输出 | ✅ | ✅ |
| `run_script` | `file_path: string` | 运行 `.m` 脚本文件 | ✅ | ✅ |
| `list_variables` | — | 列出工作区所有变量（名称、类型、维度） | ✅ `whos` 解析 | ✅ |
| `get_variable` | `name: string` | 获取变量显示值 | ✅ `disp()` 文本 | ✅ |
| `clear_workspace` | — | 清空工作区状态 | ✅ | ✅ |

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

### 下一步

- [ ] BEX 插件最小可用版
- [ ] BEX 变量读取与序列化
- [ ] 发布与安装体验完善

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
| `bxGetDoublesRO(ba)` | 只读获取浮点矩阵数据指针 |
| `bxGetDimensions(ba)` | 获取数组维度信息 |
| `bxGetClassID(ba)` | 获取变量的数据类型 |
| `bxArrayToCStr(ba, ...)` | 将变量格式化为字符串输出 |

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
