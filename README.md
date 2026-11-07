# DiabloHFConverter

Diablo 1 / Hellfire 的 MPQ 研究与精灵查看工具。**后端为纯 C/C++ 原生 `mcp-server`**，
不依赖 Node.js 运行时；通过两种传输对外提供工具：

- **stdio**：给 AI Agent（Claude Desktop / CLI 工具）用的标准 JSON-RPC 2.0。
- **WebSocket 桥**：给浏览器前端（TS / Canvas / WebGL）用，复用同一套 tool handler。

前端只负责把服务端解码好的 RGBA 帧画出来；所有 MPQ 读取与解码都在本地 C/C++ 服务端完成。

## 目录结构

| 目录 | 说明 |
|------|------|
| `src/engine/` | StormLib 封装（open / list / extract / verify） |
| `src/decoders/` | 格式解码器（palette / clx / cel / cl2 / pcx） |
| `src/tools/` | Tool Registry + `mpq_tools` + `sprite_tools` |
| `src/mcp/` | MCP 服务端（jsonrpc / stdio / ws 桥） |
| `3rdparty/StormLib/` | StormLib 源码（git submodule；构建脚本见仓库根 `build-stormlib.sh`） |
| `mpq-info/` | MPQ 文件名恢复字典与产出（参考数据，非代码） |
| `docs/` | 开发者文档（ARCHITECTURE.md / PROJECT.md） |

## 构建

先构建 StormLib 静态库（仅需一次，产物 `3rdparty/StormLib/build/libStormLib.a`）。
StormLib 以 git submodule 形式 vendored 在 `3rdparty/StormLib`，首次克隆后请先
`git submodule update --init` 拉取源码，再用仓库根的脚本构建：

```bash
bash build-stormlib.sh
```

再构建 mcp-server：

```bash
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -S . -B build-mcp
cmake --build build-mcp --target mcp-server
```

## 运行

```bash
build-mcp/mcp-server.exe --stdio                 # 给 AI Agent
build-mcp/mcp-server.exe --ws 18765              # 给浏览器前端
build-mcp/mcp-server.exe --stdio --ws 18765      # 同时启用
```

## 工具一览（使用者角度）

| 工具 | 说明 |
|------|------|
| `mpq.list` | 列出归档内文件（无 listfile 时给 `FileNNNNNNN.xxx` 占位名） |
| `mpq.extract` | 抽取单文件（返回 base64 或落盘路径） |
| `mpq.verify` | 校验某文件是否存在 |
| `mpq.recover` | 用字典 `dict_path` 恢复真实文件名 |
| `mpq.dump` | 前 N 字节十六进制预览 |
| `palette.load` | 加载 `.pal` 调色板为 RGBA（`src/decoders/palette.cpp`） |
| `sprite.decode` | 解码 `.clx` / `.cel` / `.cl2` / `.pcx` 精灵为 RGBA 帧（`src/tools/sprite_tools.cpp`） |

请求示例（JSON-RPC 2.0，stdio 与 WS 同构）：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"sprite.decode",
           "arguments":{"archive":"devilutionx.mpq","name":"data\\health.clx",
                        "palette_name":"ui_art\\diablo.pal"}}}
```

## 测试

后端各单元均配合单元测试，统一在 `out/unit_tests.cpp`，一键构建并运行：

```bash
g++ -I src -std=c++17 out/unit_tests.cpp \
    src/decoders/clx.cpp src/decoders/cel.cpp src/decoders/cl2.cpp \
    src/decoders/pcx.cpp src/decoders/palette.cpp \
    -o out/unit_tests.exe && out/unit_tests.exe
```

覆盖：CLX 解码、CEL→CLX / CL2→CLX 往返、PCX 解码、调色板加载、base64 编解码往返。
源码路径：`src/decoders/*.cpp`、`src/tools/base64.h`；测试脚本即上述 `out/unit_tests.cpp`。

## 开发文档

更详细的架构与阶段计划见 `docs/ARCHITECTURE.md` 与 `docs/PROJECT.md`。
