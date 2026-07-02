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

**当前状态**：CLI 后端已经可用，支持执行代码、运行脚本、查询变量、读取变量和清空工作区。CLI 模式通过 `.mat` 状态文件在多次 MCP 调用之间保持工作区变量；BEX 后端已经包含 JSON 协议、Python TCP 客户端和实验性 BEX 桥接插件，可在 Baltamatica GUI 进程内执行代码、运行脚本、清空工作区、查询/读取工作区变量，并触发 GUI Figure 弹窗。BEX `get_variable` 会返回文本输出，并对小型实数数值/逻辑数组返回结构化 JSON；大数组会截断，二进制矩阵传输仍在后续 PR 中完成。

---

## 🏗️ 系统架构

当前实现优先提供 **CLI fallback 后端**，通过北太天元命令行入口执行代码，并用 `.mat` 状态文件保持工作区变量。BEX 路径已定义 **JSON-over-TCP** 协议，并包含一个最小 **BEX + TCP Socket** 桥接源码，用于验证低延迟长连接后端。

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

| 特性 | CLI 后端（已实现） | BEX 后端（实验桥接） |
|:---|:---:|:---:|
| `execute_code` | ✅ | ✅ SDK `eval` |
| `run_script` | ✅ | ✅ SDK `eval("run(...)")` |
| `list_variables` | ✅ `whos` 解析 | ✅ SDK 变量枚举 |
| `get_variable` | ✅ `disp()` 文本 | ✅ 文本 + 小数组结构化 JSON |
| `clear_workspace` | ✅ | ✅ `clear` |
| 工作区状态保持 | ✅ `.mat` 状态文件 | ✅ BEX 进程长连接 |
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

- `get_variable` 已支持:数值/逻辑数组(实数**和复数**、任意大小)走 base64 二进制全保真回传;字符/字符串/结构体/元胞走结构化 JSON。极大的结构体/元胞会按上限截断(见 `truncated` 字段);结构体/元胞里嵌套的数值目前是列主序扁平数组。
- `execute_code` 暂不捕获控制台输出（计算结果请用 `get_variable` 取回）。
- 屏幕绘图可用（`figure`/`plot` 等会在 GUI 弹窗），但**图形导出到文件不可用**——当前北太天元未提供 `saveas`/`print`/`exportgraphics` 等函数；要回传图像需走数据侧（`BALTAMATICA_ARTIFACT` + CSV）或后续的绘图探针。
- 日常稳定试用仍可优先使用 CLI 后端。

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
│   ├── CMakeLists.txt      # BEX CMake 构建配置
│   ├── mcp_protocol.h      # BEX JSON 协议常量
│   └── mcp_bridge.c        # 最小 BEX TCP 桥接源码
├── tests/
│   ├── test_backend_cli.py
│   ├── test_backend_bex.py
│   ├── test_bex_sources.py
│   ├── test_integration_cli.py
│   ├── test_server.py
│   └── fixtures/sample_script.m
├── docs/
│   ├── contributing.md
│   ├── bex-protocol.md
│   ├── bex-plugin.md
│   └── pr-plan.md
└── examples/
    ├── monte_carlo_pi.m
    └── numerical_pipeline_demo.m
```

---

## 🛠️ MCP Tools（暴露给 AI 的工具接口）

| Tool 名称 | 参数 | 描述 | CLI 后端 | BEX 后端 |
|:---|:---|:---|:---:|:---:|
| `execute_code` | `code: string` | 执行代码并返回控制台输出 | ✅ | ✅ 最小桥接 |
| `run_script` | `file_path: string` | 运行 `.m` 脚本文件 | ✅ | ✅ 最小桥接 |
| `list_variables` | — | 列出工作区所有变量（名称、类型、维度） | ✅ `whos` 解析 | ✅ SDK 变量枚举 |
| `get_variable` | `name: string` | 获取变量显示值 | ✅ `disp()` 文本 | ✅ 文本 + 小数组结构化 JSON |
| `clear_workspace` | — | 清空工作区状态 | ✅ | ✅ 最小桥接 |

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

### 下一步

- [ ] BEX 大矩阵二进制传输，复数/字符/结构体/元胞的结构化序列化（目前仅文本 + 小实数/逻辑）
- [ ] BEX `execute_code` 控制台输出捕获（当前仅返回成功/错误，不含 stdout）
- [ ] BEX 图形导出到文件：北太天元缺 `saveas`/`print`/`exportgraphics`，需绘图探针（`bex/bex_plot_probe.c`）或原生导出路径
- [ ] `background` 模式跨线程调用解释器的线程安全评估
- [ ] 发布与安装体验完善（PR9：安装说明、故障排查、PyPI 元数据、BEX 二进制发布）

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
