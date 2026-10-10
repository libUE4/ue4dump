import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const tool = process.argv[2] || "adb_devices";
const args = process.argv[3] ? JSON.parse(process.argv[3]) : {};
const transport = new StdioClientTransport({
  command: "node",
  args: ["./server.mjs"],
  cwd: "/root/Andriod_UnrealMemoryTools/mcp"
});
const client = new Client({ name: "codex-local", version: "0.1.0" });
await client.connect(transport);
if (tool === "__list") {
  const r = await client.listTools();
  console.log(JSON.stringify(r, null, 2));
} else {
  const r = await client.callTool(
    { name: tool, arguments: args },
    undefined,
    { timeout: 15 * 60 * 1000, maxTotalTimeout: 15 * 60 * 1000 }
  );
  for (const c of r.content ?? []) {
    if (c.type === "text") console.log(c.text);
    else console.log(JSON.stringify(c));
  }
}
await client.close();
