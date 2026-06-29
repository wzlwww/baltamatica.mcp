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

```
┌─────────────────────────────────────────────────────────┐
│                   AI Agent (Claude Code / Cursor)       │
│                                                         │
│   "帮我生成一个 5x5 随机矩阵，求其特征值"                  │
└──────────────────────┬──────────────────────────────────┘
                       │ MCP Protocol (stdio / SSE)
                       ▼
┌─────────────────────────────────────────────────────────┐
│              Python MCP Server (baltamatica-mcp)        │
│                                                         │
│   ┌───────────┐  ┌──────────┐  ┌────────────────────┐   │
│   │ Tool:     │  │ Tool:    │  │ Tool:              │   │
│   │ exec_code │  │ list_var │  │ get_variable_value │   │
│   └─────┬─────┘  └────┬─────┘  └─────────┬──────────┘   │
│         └──────────────┴──────────────────┘              │
│                        │ TCP Socket / stdin pipe         │
└────────────────────────┼────────────────────────────────┘
                         ▼
┌─────────────────────────────────────────────────────────┐
│           Baltamatica Engine (北太天元内核)                │
│                                                         │
│   bxEvalIn()  bxGetVariableNames()  bxAddVariable()     │
│   bxGetDoublesRO()  bxCallBaltamatica()                 │
└─────────────────────────────────────────────────────────┘
```

---

## 🚀 快速开始

### 前置条件

- Python 3.10+
- [北太天元 2025](https://www.baltamatica.com/download.html)（社区版即可）
- `baltamaticaC.sh` 或 `baltamatica` 已加入系统 PATH

### 安装

```bash
git clone https://github.com/wzlwww/baltamatica.mcp.git
cd baltamatica.mcp
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
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
│       ├── engine.py           # 北太天元进程管理与通信
│       └── serializer.py       # 变量序列化（矩阵→JSON）
│
├── bex/                        # BEX 插件源码（C 语言，Phase 2）
│   ├── CMakeLists.txt          # 跨平台编译配置
│   ├── mcp_bridge.c            # BEX 插件核心：Socket 监听 + API 调用
│   └── mcp_bridge.h            # 头文件
│
├── tests/
│   ├── test_engine.py          # 引擎通信单元测试
│   ├── test_server.py          # MCP Tool 集成测试
│   └── fixtures/
│       └── sample_script.m     # 测试用 .m 脚本
│
├── docs/
│   ├── architecture.md         # 架构设计详细说明
│   ├── sdk-api-reference.md    # 北太天元 BEX SDK API 速查
│   └── contributing.md         # 贡献指南
│
└── examples/
    ├── monte_carlo_pi.m        # 蒙特卡洛估算 Pi
    └── matrix_demo.m           # 矩阵运算示例
```

---

## 🛠️ MCP Tools（暴露给 AI 的工具接口）

| Tool 名称 | 参数 | 描述 |
|:---|:---|:---|
| `execute_code` | `code: string` | 在北太天元工作区中执行一行或多行代码，返回控制台输出 |
| `run_script` | `file_path: string` | 运行指定路径的 `.m` 脚本文件 |
| `list_variables` | — | 返回当前工作区所有变量的名称、大小、类型（等效 `whos`） |
| `get_variable` | `name: string` | 获取指定变量的值，以 JSON 格式返回 |
| `clear_workspace` | — | 清空当前工作区所有变量 |

---

## 🗺️ 开发路线图 (Roadmap)

### Phase 1：基础无状态执行器 ✨
- [x] 项目结构初始化
- [ ] Python MCP Server 骨架 (`FastMCP`)
- [ ] 无状态 CLI 执行器（`baltamaticaC.sh -nodesktop -s`）
- [ ] `execute_code` 工具实现
- [ ] 基本单元测试

### Phase 2：BEX 桥接插件
- [ ] C 语言 BEX 插件编写（TCP Socket 监听）
- [ ] `bxEvalIn` / `bxGetVariableNames` 集成
- [ ] CMake 跨平台编译配置
- [ ] 插件自动加载机制

### Phase 3：状态保持交互层
- [ ] Python 端 TCP 客户端与 BEX 桥接长连接
- [ ] 变量 JSON 序列化（矩阵、结构体、元胞数组）
- [ ] `list_variables` / `get_variable` 工具实现
- [ ] 错误处理与超时机制

### Phase 4：打磨与发布
- [ ] 北太天元 IDE 内嵌 AI 状态面板（Qt）
- [ ] 远程 SSH 模式支持
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
