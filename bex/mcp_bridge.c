/*
 * mcp_bridge.c - BEX 插件：北太天元 MCP 桥接服务
 *
 * 在北太天元内部启动 TCP Socket 监听，接收 AI 代理的 JSON 指令，
 * 调用 bxEvalIn / bxGetVariableNames 等底层 API 执行计算并返回结果。
 *
 * 编译方式（在北太天元命令行中）：
 *   bex "mcp_bridge.c"
 *
 * 或使用 CMake：
 *   mkdir build && cd build && cmake .. && make
 */

// TODO: Phase 2 实现
// #include "bex/bex.h"
