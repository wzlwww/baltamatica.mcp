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

**当前状态**：CLI 后端已经可用，支持执行代码、运行脚本、查询变量、读取变量和清空工作区。CLI 模式通过 `.mat` 状态文件在多次 MCP 调用之间保持工作区变量；BEX JSON 协议与 Python TCP 客户端骨架已经就绪，真实 BEX 插件实现仍在后续规划中。

---

## 🏗️ 系统架构

当前实现优先提供 **CLI fallback 后端**，通过北太天元命令行入口执行代码，并用 `.mat` 状态文件保持工作区变量。BEX 路径已定义 **JSON-over-TCP** 协议和 Python 客户端骨架，后续将补齐真实 **BEX 插件 + TCP Socket** 后端，用于更低延迟和更高性能的数据访问。

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
│                    │                                     │
│                    ▼                                     │
│             CLI Backend                                  │
│      subprocess + .mat state file                        │
└────────────────────┬─────────────────────────────────────┘
                     ▼
        Baltamatica CLI: baltamatica -nodesktop -s "..."
```

### 后端状态

| 特性 | CLI 后端（已实现） | BEX 后端（协议/客户端骨架） |
|:---|:---:|:---:|
| `execute_code` | ✅ | 客户端协议已定义，需 BEX 插件 |
| `run_script` | ✅ | 客户端协议已定义，需 BEX 插件 |
| `list_variables` | ✅ `whos` 解析 | 客户端协议已定义，需 BEX 插件 |
| `get_variable` | ✅ `disp()` 文本 | 客户端协议已定义，需 BEX 插件 |
| `clear_workspace` | ✅ | 客户端协议已定义，需 BEX 插件 |
| 工作区状态保持 | ✅ `.mat` 状态文件 | 协议支持，长连接天然保持 |
| 图像/文件产物反馈 | ✅ 文件 artifact marker | 协议支持 artifact 列表 |

---

## 🚀 快速开始

### 前置条件

- Python 3.10+
- [北太天元 2025](https://www.baltamatica.com/download.html)（社区版即可）
- 北太天元命令行入口（macOS 通常是 `/Applications/Baltamatica.app/Contents/MacOS/baltamatica`）
- C 编译器（可选，后续 BEX 插件开发需要）

### 安装

```bash
git clone https://github.com/wzlwww/baltamatica.mcp.git
cd baltamatica.mcp
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
```

### BEX 插件状态

BEX JSON 协议见 [docs/bex-protocol.md](docs/bex-protocol.md)。Python 端已经包含
`--backend bex` 客户端骨架，可连接未来 BEX 插件提供的 TCP 服务；当前仓库的 C 插件仍是占位源码，日常试用请先使用 CLI 后端。

```bash
python -m baltamatica_mcp --backend bex --bex-host 127.0.0.1 --bex-port 31415
```

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
│   ├── backend_bex.py      # BEX JSON-over-TCP 客户端骨架
│   └── serializer.py       # 后续变量序列化占位
├── bex/
│   └── mcp_bridge.c        # 后续 BEX 插件占位
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
    ├── monte_carlo_pi.m
    └── numerical_pipeline_demo.m
```

---

## 🛠️ MCP Tools（暴露给 AI 的工具接口）

| Tool 名称 | 参数 | 描述 | CLI 后端 | BEX 后端 |
|:---|:---|:---|:---:|:---:|
| `execute_code` | `code: string` | 执行代码并返回控制台输出 | ✅ | 客户端骨架 |
| `run_script` | `file_path: string` | 运行 `.m` 脚本文件 | ✅ | 客户端骨架 |
| `list_variables` | — | 列出工作区所有变量（名称、类型、维度） | ✅ `whos` 解析 | 客户端骨架 |
| `get_variable` | `name: string` | 获取变量显示值 | ✅ `disp()` 文本 | 客户端骨架 |
| `clear_workspace` | — | 清空工作区状态 | ✅ | 客户端骨架 |

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
