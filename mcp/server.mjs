#!/usr/bin/env node
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { execFile } from "node:child_process";
import { readFile, stat } from "node:fs/promises";
import net from "node:net";
import path from "node:path";

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const localBinary = path.join(repoRoot, "outputs", "arm64-v8a", "UnrealMemoryTools");
const remoteBinary = "/data/local/tmp/UnrealMemoryTools";
const defaultPackage = "com.tencent.tmgp.dfm";
const localDumpDir = "/root/workspace/dump";
const finishedDumpDir = "/root/workspace/finished-dump/com.tencent.tmgp.dfm";

// 设备端 --mcp 服务的回环端口，经 adb forward 暴露到本机同号端口
const RPC_PORT = 35515;

function run(command, args, options = {}) {
  return new Promise((resolve) => {
    execFile(command, args, {
      cwd: repoRoot,
      timeout: options.timeout ?? 120000,
      maxBuffer: options.maxBuffer ?? 20 * 1024 * 1024
    }, (error, stdout, stderr) => {
      resolve({
        ok: !error,
        code: error?.code ?? 0,
        signal: error?.signal ?? null,
        stdout: stdout ?? "",
        stderr: stderr ?? "",
        message: error?.message ?? ""
      });
    });
  });
}

function textResult(text) {
  return { content: [{ type: "text", text }] };
}

async function adb(args, options = {}) {
  return run("adb", args, options);
}

async function adbShell(command, options = {}) {
  const escaped = command.replace(/'/g, "'\\''");
  const rooted = await adb(["shell", `su -c '${escaped}'`], options);
  if (rooted.ok || !/su: inaccessible|su: not found|inaccessible or not found|Permission denied/i.test(`${rooted.stdout}${rooted.stderr}${rooted.message}`)) {
    return rooted;
  }
  return adb(["shell", `sh -c '${escaped}'`], options);
}

async function pushBinary() {
  const exists = await stat(localBinary).then(() => true, () => false);
  if (!exists) {
    return { ok: false, output: `本地二进制不存在: ${localBinary}` };
  }

  const push = await adb(["push", localBinary, remoteBinary], { timeout: 120000 });
  if (!push.ok) {
    return { ok: false, output: `adb push 失败\n${push.stdout}${push.stderr}${push.message}` };
  }

  const chmod = await adbShell(`chmod 777 ${remoteBinary}`, { timeout: 30000 });
  if (!chmod.ok) {
    return { ok: false, output: `chmod 失败\n${chmod.stdout}${chmod.stderr}${chmod.message}` };
  }

  return { ok: true, output: `${push.stdout}${chmod.stdout}` };
}

// ------------------------------------------------------------------ RPC
// 设备端 --mcp 是 TCP + NDJSON：一行一个 JSON 请求/响应。
// 同一进程内的工具调用复用一条长连接，避免反复起停服务端。

let rpcSocket = null;
let rpcBuffer = "";
let rpcSeq = 0;
const rpcPending = new Map();
let rpcServiceStarted = false;

async function ensureForward() {
  // 端口转发会随 adb daemon 重启丢失，每次建连前都重设一次
  await adb(["forward", `tcp:${RPC_PORT}`, `tcp:${RPC_PORT}`], { timeout: 30000 });
}

async function ensureService() {
  if (rpcServiceStarted) return;

  const pushed = await pushBinary();
  if (!pushed.ok) throw new Error(pushed.output);

  // 已在跑就不重复启动
  const alive = await adbShell(`pidof UnrealMemoryTools`, { timeout: 30000 });
  if (!alive.stdout.trim()) {
    await adbShell(`cd /data/local/tmp && nohup ./UnrealMemoryTools --mcp > /data/local/tmp/umt-mcp.log 2>&1 &`, {
      timeout: 30000
    });
    // 等待监听就绪
    for (let i = 0; i < 20; i++) {
      await new Promise((r) => setTimeout(r, 500));
      const listening = await adbShell(`netstat -tln 2>/dev/null | grep ${RPC_PORT}`, { timeout: 20000 });
      if (listening.stdout.trim()) break;
    }
  }

  await ensureForward();
  rpcServiceStarted = true;
}

function attachRpcHandlers(socket) {
  socket.on("data", (chunk) => {
    rpcBuffer += chunk.toString("utf8");
    let idx;
    while ((idx = rpcBuffer.indexOf("\n")) !== -1) {
      const line = rpcBuffer.slice(0, idx).trim();
      rpcBuffer = rpcBuffer.slice(idx + 1);
      if (!line) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch {
        continue;
      }
      // hello 帧没有 id，忽略
      if (msg.id === undefined || msg.id === null) continue;
      const pending = rpcPending.get(msg.id);
      if (!pending) continue;
      rpcPending.delete(msg.id);
      pending.resolve(msg);
    }
  });

  socket.on("error", (err) => {
    for (const [, p] of rpcPending) p.reject(err);
    rpcPending.clear();
    rpcSocket = null;
    rpcServiceStarted = false;
  });

  socket.on("close", () => {
    for (const [, p] of rpcPending) p.reject(new Error("设备端连接已断开"));
    rpcPending.clear();
    rpcSocket = null;
    rpcServiceStarted = false;
  });
}

async function connectRpc() {
  if (rpcSocket) return rpcSocket;

  await ensureService();

  rpcSocket = await new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: "127.0.0.1", port: RPC_PORT }, () => {
      attachRpcHandlers(socket);
      resolve(socket);
    });
    socket.setTimeout(600000, () => socket.destroy(new Error("RPC 超时")));
    socket.once("error", reject);
  });

  rpcBuffer = "";
  return rpcSocket;
}

async function rpcCall(cmd, args = {}, timeoutMs = 600000) {
  const socket = await connectRpc();
  rpcSeq += 1;
  const id = rpcSeq;

  const payload = JSON.stringify({ id, cmd, args }) + "\n";

  const response = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      rpcPending.delete(id);
      reject(new Error(`命令 ${cmd} 超时`));
    }, timeoutMs);

    rpcPending.set(id, {
      resolve: (msg) => {
        clearTimeout(timer);
        resolve(msg);
      },
      reject: (err) => {
        clearTimeout(timer);
        reject(err);
      }
    });

    socket.write(payload, (err) => {
      if (err) {
        clearTimeout(timer);
        rpcPending.delete(id);
        reject(err);
      }
    });
  });

  return response;
}

function rpcText(response) {
  return textResult(JSON.stringify(response, null, 2));
}

// ------------------------------------------------------------------ 分析
async function analyzeDump(dumpDir, referenceDir) {
  const readIfExists = async (p) => {
    try {
      return await readFile(p, "utf8");
    } catch {
      return null;
    }
  };

  const files = [
    "AIOHeader.hpp",
    "Logs.txt",
    "Objects.txt",
    "Offsets.hpp",
    "SDK_Classes.hpp",
    "SDK_Enums.hpp",
    "SDK_Offset.hpp",
    "SDK_Structs.hpp",
    "script.json"
  ];

  const lines = [];
  for (const name of files) {
    const full = path.join(dumpDir, name);
    const info = await stat(full).then((s) => s, () => null);
    if (!info) {
      lines.push(`${name}: 缺失`);
      continue;
    }
    const ref = await stat(path.join(referenceDir, name)).then((s) => s, () => null);
    const grow = ref ? ` (参考 ${ref.size}，差 ${info.size - ref.size})` : "";
    lines.push(`${name}: ${info.size} 字节${grow}`);
  }

  const offsets = await readIfExists(path.join(dumpDir, "Offsets.hpp"));
  if (offsets) {
    const key = offsets
      .split(/\r?\n/)
      .filter((l) => /GNames|FNamePool|GUObject|ProcessEvent|Physx|Matrix/i.test(l))
      .slice(0, 12);
    if (key.length) {
      lines.push("", "关键偏移:");
      lines.push(...key.map((l) => "  " + l.trim()));
    }
  }

  return lines.join("\n");
}

// ------------------------------------------------------------------ 服务
const server = new McpServer({
  name: "unreal-memory-tools",
  version: "0.2.0"
});

server.tool(
  "adb_devices",
  "列出 adb 设备。",
  {},
  async () => {
    const result = await adb(["devices"], { timeout: 30000 });
    return textResult(`${result.stdout}${result.stderr}${result.message}`);
  }
);

server.tool(
  "list_processes",
  "列出 Android 设备上的 UE 进程（设备端 --list-processes）。",
  {},
  async () => {
    const pushed = await pushBinary();
    if (!pushed.ok) return textResult(pushed.output);
    const result = await adbShell(`${remoteBinary} --list-processes`, { timeout: 60000 });
    return textResult(`${result.stdout}${result.stderr}${result.message}`);
  }
);

server.tool(
  "dump_package",
  "headless dump 指定包名，把产物拉到本地并对关键文件做体积对比。",
  {
    packageName: z.string().default(defaultPackage),
    outputDir: z.string().default(localDumpDir)
  },
  async ({ packageName, outputDir }) => {
    const pushed = await pushBinary();
    if (!pushed.ok) return textResult(pushed.output);

    const remoteOutput = `/sdcard/UnrealMemoryTools/${packageName}`;
    await adbShell(`rm -rf ${remoteOutput}`, { timeout: 30000 });

    const dump = await adbShell(`${remoteBinary} --headless-dump ${packageName}`, {
      timeout: 10 * 60 * 1000,
      maxBuffer: 60 * 1024 * 1024
    });

    if (!dump.ok) {
      return textResult(`dump 执行失败\n${dump.stdout}${dump.stderr}${dump.message}`);
    }

    await run("rm", ["-rf", outputDir], { timeout: 30000 });
    const pull = await adb(["pull", remoteOutput, outputDir], {
      timeout: 5 * 60 * 1000,
      maxBuffer: 60 * 1024 * 1024
    });

    const analysis = await analyzeDump(outputDir, finishedDumpDir);
    return textResult([
      "dump 输出:",
      dump.stdout,
      dump.stderr,
      "pull 输出:",
      pull.stdout,
      pull.stderr,
      "分析:",
      analysis
    ].join("\n"));
  }
);

server.tool(
  "analyze_dump",
  "分析本地 dump 目录并与成品目录对比。",
  {
    dumpDir: z.string().default(localDumpDir),
    referenceDir: z.string().default(finishedDumpDir)
  },
  async ({ dumpDir, referenceDir }) => {
    const analysis = await analyzeDump(dumpDir, referenceDir);
    return textResult(analysis);
  }
);

server.tool(
  "probe_dfm_world_slots",
  "读取 DFM 当前进程的常见 GWorld 槽位，判断成品槽位是否已加载。",
  {},
  async () => {
    const pidResult = await adbShell("pidof com.tencent.tmgp.dfm", { timeout: 30000 });
    const pid = pidResult.stdout.trim().split(/\s+/)[0];
    if (!pid) return textResult(`未找到进程\n${pidResult.stdout}${pidResult.stderr}${pidResult.message}`);

    const maps = await adbShell(`grep -m1 'libUE4.so' /proc/${pid}/maps`, { timeout: 30000 });
    const firstMap = maps.stdout.trim().split(/\r?\n/)[0] ?? "";
    const baseMatch = firstMap.match(/^([0-9a-fA-F]+)-/);
    if (!baseMatch) return textResult(`无法读取 libUE4.so base\n${maps.stdout}${maps.stderr}${maps.message}`);

    const base = Number.parseInt(baseMatch[1], 16);
    const slots = [
      ["current_login_slot", 0x1d0e8668],
      ["finished_iris_slot", 0x1c9c0a50],
      ["engine_slot", 0x1d0e50b0]
    ];
    const out = [`pid=${pid}`, `base=0x${base.toString(16)}`, firstMap, ""];
    for (const [name, off] of slots) {
      const addr = base + off;
      const read = await adbShell(`dd if=/proc/${pid}/mem bs=1 skip=${addr} count=8 2>/dev/null | od -An -tx8`, { timeout: 30000 });
      out.push(`${name}: slot=base+0x${off.toString(16)} addr=0x${addr.toString(16)} value=${read.stdout.trim() || "<empty>"}`);
    }
    return textResult(out.join("\n"));
  }
);

// ------------------------------------------------------------- 设备端 RPC
server.tool(
  "rpc_ping",
  "检查设备端 MCP 服务是否就绪（必要时自动推送二进制并启动 --mcp）。",
  {},
  async () => {
    try {
      const res = await rpcCall("PING", {}, 60000);
      return rpcText(res);
    } catch (err) {
      return textResult(`RPC 不可用: ${err.message}`);
    }
  }
);

server.tool(
  "rpc_capabilities",
  "列出设备端支持的全部 RPC 命令。",
  {},
  async () => {
    try {
      return rpcText(await rpcCall("GET_CAPABILITIES", {}, 60000));
    } catch (err) {
      return textResult(`RPC 失败: ${err.message}`);
    }
  }
);

server.tool(
  "start_probe",
  "对目标包启动探针（异步）。用 probe_status 轮询结果。",
  {
    packageName: z.string().default(defaultPackage)
  },
  async ({ packageName }) => {
    try {
      return rpcText(await rpcCall("START_PROBE", { package: packageName }, 120000));
    } catch (err) {
      return textResult(`探针启动失败: ${err.message}`);
    }
  }
);

server.tool(
  "probe_status",
  "查询探针进度与结果。",
  {},
  async () => {
    try {
      return rpcText(await rpcCall("GET_PROBE_STATUS", {}, 60000));
    } catch (err) {
      return textResult(`查询失败: ${err.message}`);
    }
  }
);

server.tool(
  "probe_results",
  "读取已探测到的引擎偏移（GNames / GUObjectArray / ProcessEvent 等）。",
  {},
  async () => {
    try {
      return rpcText(await rpcCall("GET_PROBE_RESULTS", {}, 60000));
    } catch (err) {
      return textResult(`读取失败: ${err.message}`);
    }
  }
);

server.tool(
  "start_dump",
  "启动完整 SDK dump（异步）。用 dump_status 轮询。",
  {},
  async () => {
    try {
      return rpcText(await rpcCall("START_DUMP", {}, 120000));
    } catch (err) {
      return textResult(`dump 启动失败: ${err.message}`);
    }
  }
);

server.tool(
  "dump_status",
  "查询 dump 进度。",
  {},
  async () => {
    try {
      return rpcText(await rpcCall("GET_DUMP_STATUS", {}, 60000));
    } catch (err) {
      return textResult(`查询失败: ${err.message}`);
    }
  }
);

server.tool(
  "search_classes",
  "按名字模糊搜 UClass。",
  {
    nameFilter: z.string(),
    maxResults: z.number().int().min(1).max(1000).default(100),
    caseSensitive: z.boolean().default(false)
  },
  async ({ nameFilter, maxResults, caseSensitive }) => {
    try {
      return rpcText(await rpcCall("SEARCH_CLASSES", { nameFilter, maxResults, caseSensitive }, 180000));
    } catch (err) {
      return textResult(`搜索失败: ${err.message}`);
    }
  }
);

server.tool(
  "search_fields",
  "全局按字段名搜，反查「哪个类的哪个字段」，返回偏移与类型。",
  {
    nameFilter: z.string(),
    maxResults: z.number().int().min(1).max(1000).default(100),
    caseSensitive: z.boolean().default(false)
  },
  async ({ nameFilter, maxResults, caseSensitive }) => {
    try {
      return rpcText(await rpcCall("SEARCH_FIELDS", { nameFilter, maxResults, caseSensitive }, 300000));
    } catch (err) {
      return textResult(`搜索失败: ${err.message}`);
    }
  }
);

server.tool(
  "describe_class",
  "读一个 UClass 的完整定义：属性偏移 + 函数名 + native 入口 + 参数。",
  {
    className: z.string().optional(),
    classAddress: z.string().optional(),
    maxFunctions: z.number().int().min(1).max(4096).default(512)
  },
  async ({ className, classAddress, maxFunctions }) => {
    if (!className && !classAddress) return textResult("需提供 className 或 classAddress");
    try {
      return rpcText(await rpcCall("DESCRIBE_CLASS", { className, classAddress, maxFunctions }, 180000));
    } catch (err) {
      return textResult(`读取失败: ${err.message}`);
    }
  }
);

server.tool(
  "find_objects",
  "按类枚举存活实例（默认跳过 CDO）。",
  {
    className: z.string().optional(),
    classAddress: z.string().optional(),
    maxResults: z.number().int().min(1).max(2000).default(100),
    skipDefaultObjects: z.boolean().default(true)
  },
  async ({ className, classAddress, maxResults, skipDefaultObjects }) => {
    if (!className && !classAddress) return textResult("需提供 className 或 classAddress");
    try {
      return rpcText(await rpcCall("FIND_OBJECTS_BY_CLASS", {
        className, classAddress, maxResults, skipDefaultObjects
      }, 300000));
    } catch (err) {
      return textResult(`枚举失败: ${err.message}`);
    }
  }
);

server.tool(
  "read_field",
  "按字段名读对象字段值（自动沿继承链解析偏移）。",
  {
    address: z.string(),
    field: z.string(),
    size: z.number().int().min(0).max(4096).default(0)
  },
  async ({ address, field, size }) => {
    try {
      return rpcText(await rpcCall("READ_FIELD", { address, field, size }, 120000));
    } catch (err) {
      return textResult(`读取失败: ${err.message}`);
    }
  }
);

server.tool(
  "write_field",
  "按字段名写对象字段值。写内存不可逆，必须显式 confirmDangerous=true。",
  {
    address: z.string(),
    field: z.string(),
    value: z.union([z.number(), z.string(), z.boolean()]),
    size: z.number().int().min(0).max(8).default(0),
    confirmDangerous: z.boolean().default(false)
  },
  async ({ address, field, value, size, confirmDangerous }) => {
    try {
      return rpcText(await rpcCall("WRITE_FIELD", {
        address, field, value, size, confirmDangerous
      }, 120000));
    } catch (err) {
      return textResult(`写入失败: ${err.message}`);
    }
  }
);

server.tool(
  "dump_object",
  "展开对象的全部字段值，含 TArray 元素与结构体分量。",
  {
    address: z.string(),
    depth: z.number().int().min(0).max(4).default(1),
    direct: z.boolean().default(false),
    arrayLimit: z.number().int().min(0).max(256).default(16),
    structLimit: z.number().int().min(0).max(64).default(8)
  },
  async ({ address, depth, direct, arrayLimit, structLimit }) => {
    try {
      return rpcText(await rpcCall("DUMP_OBJECT", {
        address, depth, direct, arrayLimit, structLimit
      }, 180000));
    } catch (err) {
      return textResult(`展开失败: ${err.message}`);
    }
  }
);

server.tool(
  "follow_pointer_chain",
  "从某个对象地址出发走多级字段路径，支持 \"LocalPlayers[0].Pawn\" 写法。",
  {
    baseAddress: z.string(),
    fields: z.array(z.string()).min(1)
  },
  async ({ baseAddress, fields }) => {
    try {
      return rpcText(await rpcCall("FOLLOW_POINTER_CHAIN", { baseAddress, fields }, 180000));
    } catch (err) {
      return textResult(`追踪失败: ${err.message}`);
    }
  }
);

server.tool(
  "read_memory",
  "直接读目标进程内存，返回十六进制。",
  {
    address: z.string(),
    size: z.number().int().min(1).max(65536).default(16)
  },
  async ({ address, size }) => {
    try {
      return rpcText(await rpcCall("MEMORY_READ", { address, size }, 120000));
    } catch (err) {
      return textResult(`读取失败: ${err.message}`);
    }
  }
);

server.tool(
  "get_logs",
  "读设备端运行时日志尾部。",
  {
    maxLines: z.number().int().min(1).max(500).default(100)
  },
  async ({ maxLines }) => {
    try {
      return rpcText(await rpcCall("GET_LOGS", { maxLines }, 60000));
    } catch (err) {
      return textResult(`读取日志失败: ${err.message}`);
    }
  }
);

const transport = new StdioServerTransport();
await server.connect(transport);
