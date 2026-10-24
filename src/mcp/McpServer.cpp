#include "McpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "../Utils/Logger.hpp"

namespace Ue4Mcp
{
namespace
{
using json = nlohmann::json;

constexpr const char *kDefaultBind = "127.0.0.1";
constexpr size_t kMaxFrameSize = 1024 * 1024;
constexpr size_t kRecvChunkSize = 8192;

HostHooks gHooks;

// 错误码：区分协议层与执行层，便于调用方定位
constexpr const char *kBadJson = "E_BAD_JSON";
constexpr const char *kUnknownCmd = "E_UNKNOWN_CMD";
constexpr const char *kBadArgs = "E_BAD_ARGS";
constexpr const char *kNotReady = "E_NOT_READY";
constexpr const char *kInternal = "E_INTERNAL";

bool SendLine(int fd, const std::string &payload)
{
    std::string line = payload;
    line.push_back('\n');
    size_t sent = 0;
    while (sent < line.size())
    {
        const ssize_t n = ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string OkReply(const json &id, const std::string &dataJsonText)
{
    json reply;
    reply["id"] = id;
    reply["ok"] = true;
    // 宿主返回的已是 JSON 文本，避免二次转义
    try
    {
        reply["data"] = dataJsonText.empty() ? json::object() : json::parse(dataJsonText);
    }
    catch (...)
    {
        reply["data"] = json{{"raw", dataJsonText}};
    }
    return reply.dump();
}

std::string ErrReply(const json &id, const char *code, const std::string &msg)
{
    json reply;
    reply["id"] = id;
    reply["ok"] = false;
    reply["error"] = json{{"code", code}, {"msg", msg}};
    return reply.dump();
}

// 解析 "0x7c1040f000" / "7c1040f000" 两种写法
bool ParseAddress(const std::string &text, uintptr_t &out)
{
    if (text.empty())
        return false;
    const char *begin = text.c_str();
    int base = 16;
    if (begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X'))
        begin += 2;
    if (*begin == '\0')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(begin, &end, base);
    if (errno != 0 || end == begin || *end != '\0')
        return false;
    out = static_cast<uintptr_t>(value);
    return true;
}

// 单条命令分发。返回 true 表示连接可继续。
bool HandleFrame(int clientFd, const std::string &line)
{
    json request;
    try
    {
        request = json::parse(line);
    }
    catch (...)
    {
        return SendLine(clientFd, ErrReply(nullptr, kBadJson, "请求不是合法 JSON"));
    }

    const json id = request.contains("id") ? request["id"] : json(nullptr);
    if (!request.contains("cmd") || !request["cmd"].is_string())
        return SendLine(clientFd, ErrReply(id, kBadArgs, "缺少 cmd 字段"));

    const std::string cmd = request["cmd"].get<std::string>();
    const json args = request.contains("args") && request["args"].is_object()
                          ? request["args"]
                          : json::object();

    LOGI("[MCP·调用] %s", cmd.c_str());

    try
    {
        if (cmd == "PING")
        {
            const std::string build = gHooks.buildVersion ? gHooks.buildVersion() : "unknown";
            return SendLine(clientFd, OkReply(id, json{{"build", build},
                                                       {"protocol", 1},
                                                       {"connected", true}}.dump()));
        }

        if (cmd == "LIST_PROCESSES")
        {
            if (!gHooks.listProcesses)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 listProcesses"));
            return SendLine(clientFd, OkReply(id, gHooks.listProcesses()));
        }

        if (cmd == "SELECT_PROCESS")
        {
            if (!gHooks.selectProcess)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 selectProcess"));
            const int pid = args.value("pid", 0);
            const std::string package = args.value("package", "");
            if (pid <= 0 && package.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "需提供 pid 或 package"));
            return SendLine(clientFd, OkReply(id, gHooks.selectProcess(pid, package)));
        }

        if (cmd == "START_PROBE")
        {
            if (!gHooks.startProbe)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 startProbe"));
            const int pid = args.value("pid", 0);
            const std::string package = args.value("package", "");
            if (pid <= 0 && package.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "需提供 pid 或 package"));
            return SendLine(clientFd, OkReply(id, gHooks.startProbe(pid, package)));
        }

        if (cmd == "GET_PROBE_STATUS")
        {
            if (!gHooks.probeStatus)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 probeStatus"));
            return SendLine(clientFd, OkReply(id, gHooks.probeStatus()));
        }

        if (cmd == "GET_PROBE_RESULTS")
        {
            if (!gHooks.probeResults)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 probeResults"));
            return SendLine(clientFd, OkReply(id, gHooks.probeResults()));
        }

        if (cmd == "START_DUMP" || cmd == "DUMP_SDK")
        {
            if (!gHooks.startDump)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 startDump"));
            return SendLine(clientFd, OkReply(id, gHooks.startDump()));
        }

        if (cmd == "GET_DUMP_STATUS")
        {
            if (!gHooks.dumpStatus)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 dumpStatus"));
            return SendLine(clientFd, OkReply(id, gHooks.dumpStatus()));
        }

        if (cmd == "MEMORY_READ")
        {
            if (!gHooks.readMemory)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 readMemory"));
            const std::string address = args.value("address", "");
            const int size = args.value("size", 0);
            if (address.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "address 不能为空"));
            if (size <= 0 || size > 0x10000)
                return SendLine(clientFd, ErrReply(id, kBadArgs, "size 须在 [1, 65536]"));
            const std::string hex = gHooks.readMemory(address, static_cast<size_t>(size));
            if (hex.empty())
                return SendLine(clientFd, ErrReply(id, "E_READ_FAILED",
                                                  "读取失败: " + address));
            return SendLine(clientFd, OkReply(id, json{{"address", address},
                                                       {"size", size},
                                                       {"hex", hex}}.dump()));
        }

        if (cmd == "DESCRIBE_CLASS")
        {
            if (!gHooks.describeClass)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 describeClass"));
            const std::string className = args.value("className", "");
            const std::string classAddress = args.value("classAddress", "");
            if (className.empty() && classAddress.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "需提供 className 或 classAddress"));
            const int maxFunctions = args.value("maxFunctions", 512);
            return SendLine(clientFd, OkReply(id,
                gHooks.describeClass(className, classAddress, maxFunctions)));
        }

        if (cmd == "FIND_OBJECTS_BY_CLASS")
        {
            if (!gHooks.findObjects)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 findObjects"));
            const std::string className = args.value("className", "");
            const std::string classAddress = args.value("classAddress", "");
            if (className.empty() && classAddress.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "需提供 className 或 classAddress"));
            const int maxResults = args.value("maxResults", 100);
            const bool skipDefaultObjects = args.value("skipDefaultObjects", true);
            return SendLine(clientFd, OkReply(id,
                gHooks.findObjects(className, classAddress, maxResults, skipDefaultObjects)));
        }

        if (cmd == "READ_FIELD")
        {
            if (!gHooks.readField)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 readField"));
            const std::string address = args.value("address", "");
            const std::string field = args.value("field", "");
            if (address.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "address 不能为空"));
            if (field.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "field 不能为空"));
            const int size = args.value("size", 0);
            return SendLine(clientFd, OkReply(id, gHooks.readField(address, field, size)));
        }

        if (cmd == "WRITE_FIELD")
        {
            if (!gHooks.writeField)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 writeField"));
            const std::string address = args.value("address", "");
            const std::string field = args.value("field", "");
            if (address.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "address 不能为空"));
            if (field.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "field 不能为空"));
            if (!args.contains("value"))
                return SendLine(clientFd, ErrReply(id, kBadArgs, "value 不能为空"));
            const int size = args.value("size", 0);
            const bool confirmDangerous = args.value("confirmDangerous", false);
            return SendLine(clientFd, OkReply(id,
                gHooks.writeField(address, field, args["value"], size, confirmDangerous)));
        }

        if (cmd == "DUMP_OBJECT")
        {
            if (!gHooks.dumpObject)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 dumpObject"));
            const std::string address = args.value("address", "");
            if (address.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "address 不能为空"));
            const int depth = args.value("depth", 1);
            const bool direct = args.value("direct", false);
            const int arrayLimit = args.value("arrayLimit", 16);
            const int structLimit = args.value("structLimit", 8);
            return SendLine(clientFd, OkReply(id,
                gHooks.dumpObject(address, depth, direct, arrayLimit, structLimit)));
        }

        if (cmd == "FOLLOW_POINTER_CHAIN")
        {
            if (!gHooks.followChain)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 followChain"));
            const std::string baseAddress = args.value("baseAddress", "");
            if (baseAddress.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "baseAddress 不能为空"));
            if (!args.contains("fields") || !args["fields"].is_array() || args["fields"].empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "fields 须为非空字符串数组"));
            std::vector<std::string> fields;
            for (const auto &f : args["fields"])
            {
                if (!f.is_string())
                    return SendLine(clientFd, ErrReply(id, kBadArgs, "fields 元素须为字符串"));
                fields.push_back(f.get<std::string>());
            }
            return SendLine(clientFd, OkReply(id, gHooks.followChain(baseAddress, fields)));
        }

        if (cmd == "SEARCH_CLASSES")
        {
            if (!gHooks.searchClasses)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 searchClasses"));
            const std::string filter = args.value("nameFilter", "");
            if (filter.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "nameFilter 不能为空"));
            const int maxResults = args.value("maxResults", 100);
            const bool caseSensitive = args.value("caseSensitive", false);
            return SendLine(clientFd, OkReply(id,
                gHooks.searchClasses(filter, maxResults, caseSensitive)));
        }

        if (cmd == "SEARCH_FIELDS")
        {
            if (!gHooks.searchFields)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 searchFields"));
            const std::string filter = args.value("nameFilter", "");
            if (filter.empty())
                return SendLine(clientFd, ErrReply(id, kBadArgs, "nameFilter 不能为空"));
            const int maxResults = args.value("maxResults", 100);
            const bool caseSensitive = args.value("caseSensitive", false);
            return SendLine(clientFd, OkReply(id,
                gHooks.searchFields(filter, maxResults, caseSensitive)));
        }

        if (cmd == "GET_LOGS")
        {
            if (!gHooks.getLogs)
                return SendLine(clientFd, ErrReply(id, kNotReady, "宿主未提供 getLogs"));
            const int maxLines = args.value("maxLines", 100);
            return SendLine(clientFd, OkReply(id, gHooks.getLogs(maxLines)));
        }

        if (cmd == "GET_CAPABILITIES")
        {
            json caps = json::array();
            if (gHooks.listProcesses) caps.push_back("LIST_PROCESSES");
            if (gHooks.selectProcess) caps.push_back("SELECT_PROCESS");
            if (gHooks.startProbe) caps.push_back("START_PROBE");
            if (gHooks.probeStatus) caps.push_back("GET_PROBE_STATUS");
            if (gHooks.probeResults) caps.push_back("GET_PROBE_RESULTS");
            if (gHooks.startDump) { caps.push_back("START_DUMP"); caps.push_back("DUMP_SDK"); }
            if (gHooks.dumpStatus) caps.push_back("GET_DUMP_STATUS");
            if (gHooks.readMemory) caps.push_back("MEMORY_READ");
            if (gHooks.describeClass) caps.push_back("DESCRIBE_CLASS");
            if (gHooks.findObjects) caps.push_back("FIND_OBJECTS_BY_CLASS");
            if (gHooks.readField) caps.push_back("READ_FIELD");
            if (gHooks.writeField) caps.push_back("WRITE_FIELD");
            if (gHooks.dumpObject) caps.push_back("DUMP_OBJECT");
            if (gHooks.followChain) caps.push_back("FOLLOW_POINTER_CHAIN");
            if (gHooks.searchClasses) caps.push_back("SEARCH_CLASSES");
            if (gHooks.searchFields) caps.push_back("SEARCH_FIELDS");
            if (gHooks.getLogs) caps.push_back("GET_LOGS");
            caps.push_back("PING");
            caps.push_back("GET_CAPABILITIES");
            return SendLine(clientFd, OkReply(id, json{{"capabilities", caps}}.dump()));
        }

        return SendLine(clientFd, ErrReply(id, kUnknownCmd, "未知命令: " + cmd));
    }
    catch (const std::exception &e)
    {
        return SendLine(clientFd, ErrReply(id, kInternal, std::string("内部错误: ") + e.what()));
    }
    catch (...)
    {
        return SendLine(clientFd, ErrReply(id, kInternal, "内部错误"));
    }
}
}  // namespace

void InstallHooks(HostHooks hooks)
{
    gHooks = std::move(hooks);
}

int Run(const std::string &bindAddress, unsigned short port)
{
    const std::string addr = bindAddress.empty() ? kDefaultBind : bindAddress;

    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0)
    {
        LOGE("[MCP] socket 创建失败: %s", std::strerror(errno));
        return 1;
    }

    int reuse = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addrIn{};
    addrIn.sin_family = AF_INET;
    addrIn.sin_port = htons(port);
    if (::inet_pton(AF_INET, addr.c_str(), &addrIn.sin_addr) != 1)
    {
        LOGE("[MCP] 地址解析失败: %s", addr.c_str());
        ::close(serverFd);
        return 1;
    }

    if (::bind(serverFd, reinterpret_cast<sockaddr *>(&addrIn), sizeof(addrIn)) < 0)
    {
        LOGE("[MCP] bind %s:%u 失败: %s", addr.c_str(), port, std::strerror(errno));
        ::close(serverFd);
        return 1;
    }

    if (::listen(serverFd, 1) < 0)
    {
        LOGE("[MCP] listen 失败: %s", std::strerror(errno));
        ::close(serverFd);
        return 1;
    }

    LOGI("[MCP] 命令服务已启动 %s:%u", addr.c_str(), port);

    for (;;)
    {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        const int clientFd = ::accept(serverFd, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
        if (clientFd < 0)
        {
            if (errno == EINTR)
                continue;
            LOGE("[MCP] accept 失败: %s", std::strerror(errno));
            break;
        }

        LOGI("[MCP·连接] 客户端已连接");
        ::dprintf(clientFd, "{\"type\":\"hello\",\"protocol\":1,\"build\":\"%s\"}\n",
                  gHooks.buildVersion ? gHooks.buildVersion().c_str() : "unknown");

        std::string recvBuf;
        std::vector<char> chunk(kRecvChunkSize);
        bool connected = true;

        while (connected)
        {
            const ssize_t got = ::recv(clientFd, chunk.data(), chunk.size(), 0);
            if (got <= 0)
                break;

            recvBuf.append(chunk.data(), static_cast<size_t>(got));
            if (recvBuf.size() > kMaxFrameSize * 16)
            {
                LOGE("[MCP] recvBuf 超限，断开");
                break;
            }

            size_t pos = 0;
            while ((pos = recvBuf.find('\n')) != std::string::npos)
            {
                const std::string line = recvBuf.substr(0, pos);
                recvBuf.erase(0, pos + 1);
                if (line.empty())
                    continue;
                if (line.size() > kMaxFrameSize)
                {
                    LOGE("[MCP] 单帧超限，断开");
                    connected = false;
                    break;
                }
                if (!HandleFrame(clientFd, line))
                {
                    connected = false;
                    break;
                }
            }
        }

        LOGI("[MCP·连接] 客户端断开");
        ::close(clientFd);
    }

    ::close(serverFd);
    return 0;
}
}  // namespace Ue4Mcp
