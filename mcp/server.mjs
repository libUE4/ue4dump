#!/usr/bin/env node
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { execFile } from "node:child_process";
import { readFile, stat } from "node:fs/promises";
import path from "node:path";

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const localBinary = path.join(repoRoot, "outputs", "arm64-v8a", "UnrealMemoryTools");
const remoteBinary = "/data/local/tmp/UnrealMemoryTools";
const defaultPackage = "com.tencent.tmgp.dfm";
const localDumpDir = "/root/workspace/dump";
const finishedDumpDir = "/root/workspace/finished-dump/com.tencent.tmgp.dfm";

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

function parseOffsets(text) {
  const result = {};
  const re = /constexpr\s+uintptr_t\s+([A-Za-z0-9_]+)\s*=\s*0x([0-9a-fA-F]+);/g;
  for (const match of text.matchAll(re)) {
    result[match[1]] = `0x${match[2].toLowerCase()}`;
  }
  return result;
}

async function readMaybe(file) {
  try {
    return await readFile(file, "utf8");
  } catch {
    return "";
  }
}

function interestingLog(logs) {
  return logs
    .split(/\r?\n/)
    .filter((line) => /FNamePool|GUObjectArray|ObjObjects Num|Finding GEngine|GWorld candidates|slot \[<Base>|GWorld:|GEngine:|ProcessEvent|Gathered|Saved packages|Couldn't find/.test(line))
    .join("\n");
}

async function analyzeDump(dumpDir = localDumpDir, referenceDir = finishedDumpDir) {
  const dumpOffsetsText = await readMaybe(path.join(dumpDir, "Offsets.hpp"));
  const refOffsetsText = await readMaybe(path.join(referenceDir, "Offsets.hpp"));
  const dumpLogs = await readMaybe(path.join(dumpDir, "Logs.txt"));
  const refLogs = await readMaybe(path.join(referenceDir, "Logs.txt"));

  const dumpOffsets = parseOffsets(dumpOffsetsText);
  const refOffsets = parseOffsets(refOffsetsText);
  const keys = ["Names", "UObjectArray", "ObjObjects", "Engine", "World", "Matrix", "Physx", "ProcessEvent", "ProcessEventIdx"];
  const lines = [];
  lines.push(`dump: ${dumpDir}`);
  lines.push(`reference: ${referenceDir}`);
  lines.push("");
  lines.push("关键指针对比:");
  for (const key of keys) {
    const a = dumpOffsets[key] ?? "<missing>";
    const b = refOffsets[key] ?? "<missing>";
    lines.push(`${key}: dump=${a} reference=${b} ${a === b ? "OK" : "DIFF"}`);
  }

  const classFlagsDump = dumpOffsets.ClassFlags ?? "<missing>";
  const classFlagsRef = refOffsets.ClassFlags ?? "<missing>";
  const actorsDump = dumpOffsets.Actors ?? "<missing>";
  const actorsRef = refOffsets.Actors ?? "<missing>";
  lines.push("");
  lines.push(`ClassFlags: dump=${classFlagsDump} reference=${classFlagsRef} ${classFlagsDump === classFlagsRef ? "OK" : "DIFF"}`);
  lines.push(`ULevel.Actors: dump=${actorsDump} reference=${actorsRef} ${actorsDump === actorsRef ? "OK" : "DIFF"}`);

  lines.push("");
  lines.push("dump 日志摘要:");
  lines.push(interestingLog(dumpLogs) || "<empty>");

  const dumpWorldCandidate = dumpLogs.match(/slot \[<Base> \+ 0x([0-9A-Fa-f]+)\] -> 0x[0-9A-Fa-f]+, score \d+, ([^\n]+)/);
  if (dumpWorldCandidate) {
    lines.push("");
    lines.push(`当前 GWorld 候选对象: ${dumpWorldCandidate[2].trim()}`);
    if (/World Login\.Login/.test(dumpWorldCandidate[2])) {
      lines.push("判断: 当前 dump 处于 Login 世界，通常不是成品绘制需要的局内/安全屋世界。");
    }
  }
  if (refLogs) {
    lines.push("");
    lines.push("reference 日志摘要:");
    lines.push(interestingLog(refLogs));
  }

  return lines.join("\n");
}

const server = new McpServer({
  name: "unreal-memory-tools",
  version: "0.1.0"
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
  "推送 UnrealMemoryTools 并列出 Android 设备上的 UE 进程。",
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
  "推送 UnrealMemoryTools，headless dump 指定包名，并把 /sdcard/UnrealMemoryTools/<package> 拉到本地目录。",
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
    return textResult(await analyzeDump(dumpDir, referenceDir));
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

const transport = new StdioServerTransport();
await server.connect(transport);
