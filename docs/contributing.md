# 贡献指南

感谢您对 baltamatica.mcp 项目的关注！

## 开发环境搭建

```bash
git clone https://github.com/wzlwww/baltamatica.mcp.git
cd baltamatica.mcp
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
```

## 分支模型

```
main          ← 稳定发布分支（保护分支，仅通过 PR 合入）
  └── develop ← 开发主分支（日常开发合入此处）
       ├── feature/phase1-cli-executor
       ├── feature/phase2-bex-bridge
       ├── feature/phase3-stateful
       └── fix/output-truncation
```

## 工作流程

1. 从 `develop` 拉出 feature 分支
2. 开发完成后提交 PR 到 `develop`
3. Code Review 通过后合入
4. 阶段性稳定后从 `develop` 合入 `main` 并打 tag

## 提交规范

使用 [Conventional Commits](https://www.conventionalcommits.org/)：

- `feat:` 新功能
- `fix:` Bug 修复
- `docs:` 文档更新
- `refactor:` 代码重构
- `test:` 测试相关
- `chore:` 构建/CI/工具链

## 运行测试

```bash
pytest tests/
```
