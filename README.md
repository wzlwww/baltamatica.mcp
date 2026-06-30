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

**核心特性**：所有计算在同一个北太天元进程中执行，**变量和工作区状态在多次调用之间保持**，实现真正的交互式科学计算体验。

---

## 🏗️ 系统架构

本项目默认采用 **BEX 插件 + TCP Socket** 架构，实现 AI 代理与北太天元内核之间的高性能双向通信。

```
┌──────────────────────────────────────────────────────────┐
│                AI Agent (Claude Code / Cursor / Codex)   │
│                                                          │
│    "帮我生成一个 5x5 随机矩阵，求其特征值"                  │
└─────────────────────────┬────────────────────────────────┘
                          │ MCP Protocol (stdio)
                          ▼
┌──────────────────────────────────────────────────────────┐
│               Python MCP Server (baltamatica-mcp)        │
│                                                          │
│  ┌────────────┐ ┌──────────────┐ ┌────────────────────┐  │
│  │ exec_code  │ │ list_variables│ │ get_variable_value │  │
│  └─────┬──────┘ └──────┬───────┘ └─────────┬──────────┘  │
│        └───────────────┼───────────────────┘             │
│                        │                                 │
│           ┌────────────┴────────────┐                    │
│           │    Engine Dispatcher    │                    │
│           │  (自动选择通信后端)       │                    │
│           └─────┬──────────┬────────┘                    │
│                 │          │                             │
│      ┌──────────▼──┐  ┌───▼──────────┐                  │
│      │ BEX 后端    │  │ CLI 后端     │                  │
│      │ (默认,高性能)│  │ (fallback)   │                  │
│      │ TCP Socket  │  │ subprocess   │                  │
│      └──────┬──────┘  └───┬──────────┘                  │
└─────────────┼─────────────┼──────────────────────────────┘
              │             │
              ▼             ▼
┌─────────────────────┐  ┌──────────────────────────────┐
│  BEX Plugin         │  │  baltamaticaC.sh -nodesktop  │
│  (mcp_bridge.so)    │  │  -s "command"                │
│                     │  └──────────────────────────────┘
│  bxEvalIn()         │
│  bxGetVariableNames()│
│  bxGetDoublesRO()   │
│  bxAddVariable()    │
│                     │
│  ┌───────────────┐  │
│  │ Baltamatica   │  │
│  │ Engine Core   │  │
│  └───────────────┘  │
└─────────────────────┘
```

### 两种后端对比

| 特性 | 🚀 BEX 后端（默认） | 🐢 CLI 后端（fallback） |
|:---|:---|:---|
| **通信方式** | TCP Socket 长连接 | 每次启动新进程 |
| **响应延迟** | < 1 ms | ~700 ms |
| **变量状态保持** | ✅ 天然保持 | ❌ 需 save/load |
| **大矩阵传输** | 二进制直传，极快 | 文本序列化，慢 |
| **安装门槛** | 需编译 BEX 插件 | 零编译，开箱即用 |
| **适用场景** | 日常开发、大规模计算、实时仿真 | 快速体验、无编译环境 |

---

## 🚀 快速开始

### 前置条件

- Python 3.10+
- [北太天元 2025](https://www.baltamatica.com/download.html)（社区版即可）
- C 编译器（gcc / clang / MSVC，用于编译 BEX 插件）

### 安装

```bash
git clone https://github.com/wzlwww/baltamatica.mcp.git
cd baltamatica.mcp
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
```

### 编译 BEX 插件

```bash
cd bex
mkdir build && cd build
cmake .. -DBALTAM_ROOT=/opt/Baltamatica   # Linux
# cmake .. -DBALTAM_ROOT=/Applications/Baltamatica.app/Contents  # macOS
make
# 编译产物 mcp_bridge.bexa64 (Linux) / mcp_bridge.bexmaci64 (macOS)
# 将其复制到北太天元的 plugins 目录或工作目录
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
├── README.md                   # 项目说明文档
├── LICENSE                     # MIT 开源许可证
├── pyproject.toml              # Python 项目配置 & 依赖管理
├── .gitignore                  # Git 忽略规则
│
├── src/
│   └── baltamatica_mcp/
│       ├── __init__.py         # 包初始化
│       ├── __main__.py         # 入口：python -m baltamatica_mcp
│       ├── server.py           # MCP Server 主逻辑 & Tool 注册
│       ├── engine.py           # 引擎调度器（自动选择 BEX / CLI 后端）
│       ├── backend_bex.py      # BEX 后端：TCP Socket 客户端
│       ├── backend_cli.py      # CLI 后端：subprocess 调用（fallback）
│       └── serializer.py       # 变量序列化（矩阵→JSON）
│
├── bex/                        # BEX 插件源码（C 语言）
│   ├── CMakeLists.txt          # 跨平台编译配置
│   ├── mcp_bridge.c            # 插件核心：TCP 监听 + bxEvalIn 调用
│   ├── mcp_bridge.h            # 头文件
│   └── protocol.h              # MCP ↔ BEX 通信协议定义（JSON 消息格式）
│
├── tests/
│   ├── test_backend_bex.py     # BEX 后端集成测试
│   ├── test_backend_cli.py     # CLI 后端单元测试
│   ├── test_server.py          # MCP Tool 端到端测试
│   └── fixtures/
│       └── sample_script.m     # 测试用 .m 脚本
│
├── docs/
│   ├── architecture.md         # 架构设计详细说明
│   ├── bex-build-guide.md      # BEX 插件编译指南（各平台）
│   ├── sdk-api-reference.md    # 北太天元 BEX SDK API 速查
│   └── contributing.md         # 贡献指南
│
└── examples/
    ├── monte_carlo_pi.m        # 蒙特卡洛估算 Pi
    └── matrix_demo.m           # 矩阵运算示例
```

---

## 🛠️ MCP Tools（暴露给 AI 的工具接口）

| Tool 名称 | 参数 | 描述 | BEX 后端 | CLI 后端 |
|:---|:---|:---|:---:|:---:|
| `execute_code` | `code: string` | 执行代码并返回控制台输出 | ✅ | ✅ |
| `run_script` | `file_path: string` | 运行 `.m` 脚本文件 | ✅ | ✅ |
| `list_variables` | — | 列出工作区所有变量（名称、类型、维度） | ✅ 结构化 | ✅ 解析 whos |
| `get_variable` | `name: string` | 获取变量值（JSON 格式） | ✅ 二进制直读 | ✅ 文本解析 |
| `clear_workspace` | — | 清空工作区 | ✅ | ✅ |

---

## 🗺️ 开发路线图 (Roadmap)

### Phase 1：项目骨架 & CLI Fallback 后端
- [x] 项目结构初始化
- [x] Python MCP Server 骨架（`FastMCP`）
- [x] CLI 后端实现（`subprocess` + `baltamaticaC.sh -nodesktop -s`）
- [x] `execute_code` / `run_script` 工具
- [x] `list_variables` / `get_variable` / `clear_workspace` 工具（CLI 基础版）
- [x] 基本单元测试

### Phase 2：BEX 桥接插件（核心）
- [ ] C 语言 BEX 插件：TCP Socket 监听线程
- [ ] JSON 消息协议定义（`protocol.h`）
- [ ] `bxEvalIn` 执行 / `bxGetVariableNames` 变量查询
- [ ] `bxGetDoublesRO` 大矩阵二进制序列化
- [ ] CMake 跨平台编译（Linux / macOS / Windows）
- [ ] 插件自动发现与加载

### Phase 3：BEX 后端集成 & 变量序列化
- [ ] Python 端 TCP 长连接客户端（`backend_bex.py`）
- [ ] 引擎调度器：自动检测 BEX 插件是否可用，否则降级 CLI
- [ ] 变量 JSON 序列化（矩阵、结构体、元胞数组、稀疏矩阵）
- [ ] `list_variables` / `get_variable` 工具（BEX 高性能版）
- [ ] 错误处理、超时、断线重连

### Phase 4：打磨与发布
- [ ] 远程 SSH 模式支持
- [ ] 预编译 BEX 二进制分发（GitHub Releases）
- [ ] PyPI 发布 & MCP 官方服务列表注册
- [ ] 完善文档与示例

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
