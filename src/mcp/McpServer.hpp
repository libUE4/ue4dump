#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace Ue4Mcp
{
// 主程序向 MCP 服务暴露的能力。MCP 层只做协议收发，
// 具体动作（探针 / dump / 读内存 / 枚举对象）由主程序注入。
struct HostHooks
{
    // 回显构建版本
    std::function<std::string()> buildVersion;

    // 列出当前可选的 UE 进程，返回 JSON 数组文本
    std::function<std::string()> listProcesses;

    // 选中目标进程（只记录，不 attach）
    std::function<std::string(int pid, const std::string &package)> selectProcess;

    // 执行探针；返回 JSON 文本
    std::function<std::string(int pid, const std::string &package)> startProbe;

    // 查询探针阶段状态；返回 JSON 文本
    std::function<std::string()> probeStatus;

    // 读取已探测到的偏移与结构分组；返回 JSON 文本
    std::function<std::string()> probeResults;

    // 执行完整 dump；返回 JSON 文本
    std::function<std::string()> startDump;

    // 查询 dump 状态；返回 JSON 文本
    std::function<std::string()> dumpStatus;

    // 读目标进程内存：返回十六进制文本，失败返回空串
    std::function<std::string(const std::string &address, size_t size)> readMemory;

    // 运行时日志尾部；返回 JSON 数组文本
    std::function<std::string(int maxLines)> getLogs;
};

// 注册主程序实现。必须在 Run() 之前调用。
void InstallHooks(HostHooks hooks);

// 启动命令服务（阻塞）。bindAddress 为空时用 127.0.0.1。
int Run(const std::string &bindAddress, unsigned short port);
}  // namespace Ue4Mcp
