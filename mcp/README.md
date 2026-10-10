# UnrealMemoryTools MCP

本 MCP 服务是 `UnrealMemoryTools` 的本地 wrapper。

可用工具：

- `adb_devices`
- `list_processes`
- `dump_package`
- `analyze_dump`

启动：

```bash
cd /root/Andriod_UnrealMemoryTools/mcp
npm install
npm start
```

`dump_package` 会：

1. `adb push` 最新 `outputs/arm64-v8a/UnrealMemoryTools`
2. 在设备上执行 `UnrealMemoryTools --headless-dump <package>`
3. 拉取 `/sdcard/UnrealMemoryTools/<package>` 到 `/root/辅助项目/dump`
4. 对比 `/root/辅助项目/com.tencent.tmgp.dfm`
