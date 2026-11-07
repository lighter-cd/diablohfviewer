# 后端 / MCP 架构文档（ARCHITECTURE）

> 状态：**架构已确认；阶段 0–1 已落地（2026-11-07）**。
> 本文档描述经用户确认的目标架构。实现顺序见文末 §9 与 `PROJECT.md` 第 8 节阶段计划。
> 当前 StormLib 引擎（vendored 于 `3rdparty/StormLib`）已验证可用，是本协议架构的“引擎底座”；
> 阶段 1 解码器（`src/decoders/{palette,clx,cel,cl2,pcx}` + `sprite_tools`）已实现并通过
> 单元 + 端到端验证，前端可经 `sprite.decode` 拿到 RGBA 帧绘制精灵。

---

## 0. 一句话结论

**后端完全用 C/C++ 实现，不依赖 Node.js 运行时。** 一个本地 **MCP Server** 同时提供两种传输：

- **stdio 传输** —— 给 AI Agent（Claude Desktop / CLI 工具）用，标准 JSON-RPC 2.0。
- **WebSocket 桥接** —— 给 **TS / Canvas / WebGL 的浏览器前端**用。浏览器无法 spawn
  子进程走 stdio，所以本地 MCP Server 额外起一个 WS 服务，复用同一套 tool handler。

前端是**独立的 TS Web 程序**（Canvas/WebGL 绘制），通过 WS 与本地 MCP Server 通信，
由 Server 读取本地 MPQ 并完成解码，把数据回传前端绘制。前端**不碰文件系统、不读 MPQ**。

```
┌─────────────────────┐         ┌──────────────────────────────────────┐
│  AI Agent (stdio)   │ stdio   │                                        │
│  JSON-RPC 2.0       ├────────►│            MCP Server (C/C++)          │
└─────────────────────┘         │  ┌──────────────────────────────────┐  │
                                │  │   Tool Registry (name -> handler) │  │
┌─────────────────────┐  WS     │  │   handler(params)->JSON           │  │
│  TS/Canvas/WebGL    ├──WS────►│  └──────────────┬───────────────────┘  │
│  浏览器前端          │ JSON-RPC│                 │ dispatch              │
└─────────────────────┘         │  ┌──────────────▼───────────────────┐  │
                                │  │  src/tools  (组合 engine+decoders) │  │
                                │  │  src/decoders (CEL/CL2/CLX/PCX/..) │  │
                                │  │  src/engine   (StormLib 封装)      │  │
                                │  └──────────────┬───────────────────┘  │
                                │                 │ 原始字节              │
                                │        ┌────────▼────────┐             │
                                │        │ 本地 MPQ 文件    │             │
                                │        │ DIABDAT.MPQ ...  │             │
                                │        └─────────────────┘             │
                                └──────────────────────────────────────┘
```

---

## 1. 为什么这样设计（决策依据）

1. **底层是 C 引用实现**：MPQ 容器与编解码（StormLib）本身就是 C。手写移植到 TS 既脆弱
   又重复造轮子（已在阶段 0 验证：StormLib 经 MinGW-w64 干净编译并正确抽取）。
2. **Node.js 对 MCP Server 不是必须的**：MCP 是语言无关的 JSON-RPC 2.0 协议，官方有
   C++ SDK，社区有 C23 SDK。因此后端可以彻底脱离 Node 运行时，产物是一个
   本地原生可执行文件（体积小、启动快、无运行时依赖）。
3. **浏览器不能走 stdio**：浏览器安全模型禁止 spawn 子进程、读 stdin/stdout。因此给
   Agent 用 stdio，给浏览器用 **同一个 Server 起的 WebSocket 桥**，二者**复用同一套
   tool handler**，协议形状完全一致，只是传输不同。
4. **前端是纯 TS/Web**：绘制（Canvas 2D / WebGL）天然属于浏览器领域，用 TS 最省力；
   它只负责“拿到数据后怎么画”，所有重活（MPQ 读取、解码）都在本地 C Server。

---

## 2. 模块布局（`src/`）

```
src/
├── engine/              # MPQ 引擎封装（薄 C++ 层，包 StormLib C API）
│   ├── archive.h/.cpp   #   open/list/extract/verify，返回原始字节 + 元数据
│   └── storm_wrap.*     #   StormLib 句柄管理、错误码->字符串
│
├── decoders/            # 格式解码器（移植 DevilutionX 的 load_*）
│   ├── palette.*        #   .pal  256 色调色板 -> RGBA[256]
│   ├── cel.*            #   .cel  RLE + 调色板索引 -> RGBA 帧缓冲
│   ├── cl2.*            #   .cl2 编译型动画 -> 帧组（含 cl2_to_clx 思路）
│   ├── clx.*            #   .clx DevilutionX 内部格式
│   ├── pcx.*            #   .pcx PC Paintbrush 静态图
│   └── level.*          #   TIL/MIN/DUN 拼接 -> 瓦片地图（阶段 4）
│
├── tools/               # 工具注册表：把 engine + decoders 组合成“工具”
│   ├── registry.h/.cpp  #   工具注册、按名分发、参数 schema、返回 JSON
│   ├── mpq_tools.*      #   mpq.list / mpq.extract / mpq.verify / mpq.recover / mpq.dump
│   ├── sprite_tools.*   #   sprite.decode / palette.load
│   └── map_tools.*      #   map.build（阶段 4）
│
├── mcp/                 # MCP 服务端：传输 + 协议分派
│   ├── server.h/.cpp    #   初始化 registry、启动传输
│   ├── jsonrpc.*        #   JSON-RPC 2.0 解析/组装（基于 nlohmann/json）
│   ├── transport_stdio.*#   stdin/stdout 行分帧
│   └── transport_ws.*   #   WebSocket 桥（mongoose，或自研最小 RFC6455）
│
└── main.cpp             # 入口：--stdio / --ws <port> / 同时启用
```

> 旧的独立工具 `mpqextract`（曾位于 `tools/c/`）与 `tools/*.ts`（含 `ts-legacy/`）已随
> “TS→C 迁移 + 目录重构”**全部移除**：其能力（`list/extract/verify/checklist/dump` 及文件名
> 恢复）均已落在 `src/` 后端的 `mpq.*` 工具中，单一 `mcp-server.exe` 即为唯一入口。
> StormLib 静态库的构建脚本现位于仓库根 `build-stormlib.sh`（StormLib 本身以 git submodule
> 形式 vendored 于 `3rdparty/StormLib`）；MPQ 文件名恢复用的字典与
> 原始路径清单等参考数据现集中在 `mpq-info/`（原 `tools/`）。

---

## 3. Tool Registry（工具注册表）

每个工具是一个具名、带参数、返回 JSON 的 C 函数。注册表把名字映射到 handler，
并携带元数据（描述、输入 JSON schema），供 MCP `tools/list` 暴露给 Agent。

```cpp
// tools/registry.h
struct ToolResult { bool ok; std::string json; };          // json: 结果或错误详情
using ToolHandler = ToolResult(*)(const nlohmann::json& params);

struct ToolDef {
    std::string name;          // 如 "mpq.list"
    std::string description;   // 给 Agent 看的自然语言说明
    nlohmann::json input_schema; // JSON Schema（MCP tools/list 用）
    ToolHandler handler;
};

class ToolRegistry {
public:
    void registerTool(const ToolDef& t);
    // 通过名字分发；未知工具或参数非法都返回标准错误 JSON
    ToolResult dispatch(const std::string& name, const nlohmann::json& params) const;
    nlohmann::json list() const;   // 生成 MCP tools/list 的 tools[] 数组
};
```

**设计要点**
- handler 只接受/返回 JSON，与传输无关。stdio 与 WS 都调用同一个 `dispatch`。
- 二进制产物（解码后的图像）以 **base64** 放进 JSON 字段（如 `data_url`），
  前端直接 `atob` 成 PNG Blob 绘制。大地图如需省带宽，后续可加 HTTP 二进制路由
  `GET /blob/<id>`（见 §8 备注），当前版本先用 base64 保持简单。

---

## 4. 工具清单（目标）

| 工具名 | 参数 | 返回 | 阶段 |
|--------|------|------|------|
| `mpq.list`     | `{archive}` | `{files:[{name,size,flags}]}`（无 listfile 时给 `FileNNNNNNN.xxx` 占位） | 0 |
| `mpq.extract`  | `{archive,name}` | `{bytes_base64}` 或 `{saved_path}` | 0 |
| `mpq.verify`   | `{archive,name}` | `{exists:bool}` | 0 |
| `mpq.recover`  | `{archive,dict_path}` | `{recovered:[{name,size}]}`（移植 mpq-recover） | 0 |
| `mpq.dump`     | `{archive,name,max_bytes?}` | `{name,size,preview_hex}`（前 N 字节十六进制预览；移植 mpqextract `dump`） | 0 |
| `palette.load` | `{archive,name}` | `{name,count,colors:[[r,g,b]×256],rgba_base64}` | ✅ |
| `sprite.decode`| `{archive,name,palette?,frame?,width?}` | `{name,format,width,height,frame_count,palette_resolved,frames:[{index,width,height,rgba_base64}]}`（`.clx` 直解；`.cel`/`.cl2` 经 `celToClx`/`cl2ToClx` 转 CLX 后解；`.pcx` 走内置解码。格式由扩展名自动探测。`width` 为 `.cel`/`.cl2` 必需，阶段 2 起支持 `width:"auto"` 按路径自动查表） | ✅ |
| `sprite.list`  | `{archive?,family?,class?}` | `{sprites:[{name,archive,family,class,type,width,frames?}]}`（从 `recovered.json` + DevilutionX 数据表枚举**真实存在的**精灵名，供前端建目录树；无 listfile 时 `mpq.list` 给的是占位名，故需此工具） | 阶段 2（计划中） |
| `map.build`    | `{archive,dun,...}` | `{tiles:[...],width,height}` | 4 |

> **PCX 已并入 `sprite.decode`**（按 `.pcx` 扩展名自动路由），不再单独设 `pcx.decode` 工具。
> `sprite.decode` 的调色板解析优先级：`palette`（base64 或名称）→ 文件内嵌（PCX）→ 同目录
> `<base>.pal`；都找不到则报错。返回中 `palette_resolved` 标记是否成功解析到调色板。
> 工具名用点分命名空间，便于 Agent 理解分组，也方便前端按前缀路由 UI。

---

## 5. 传输设计（Transport）

### 5.1 stdio（Agent 用）
- 协议：**JSON-RPC 2.0**，行分帧（每行一个完整 JSON 请求/响应，`\n` 分隔）。
- 进程从 `stdin` 逐行读请求，`dispatch` 后把响应写 `stdout`，`fflush`。
- **严格**：除 JSON 外不向 stdout 写任何东西（日志走 `stderr`），否则会污染协议流。
- `Content-Length` 头分帧也可选支持（更健壮），行分帧先够用。

### 5.2 WebSocket 桥（浏览器前端用）
- Server 在 `ws://localhost:<port>` 监听（端口可配置，默认如 `18765`）。
- 帧协议：与 stdio **同构的 JSON-RPC 2.0**。浏览器 `new WebSocket(...)` 后
  `send(JSON.stringify(request))`，`onmessage` 收到 `JSON.stringify(response)`。
- WS 文本帧即可（JSON 是文本）；二进制产物走 base64 字段，无需 WS 二进制帧。
- 请求/响应用 `id` 配对（同 JSON-RPC），前端可并发多请求。
- 可加 `initialize`/`ping` 等 MCP 握手可选支持；最小可用版直接接受
  `tools/list` 与 `tools/call` 两个 method 即可。

### 5.3 启动方式（`main.cpp`）
```
mcp-server --stdio                 # 仅 stdio（给 Agent，作为子进程拉起）
mcp-server --ws 18765              # 仅 WS（给前端，后台常驻）
mcp-server --stdio --ws 18765      # 同时（开发调试最方便）
```

---

## 6. 前端 WS 契约（约定）

前端（TS/Canvas/WebGL）与本地 Server 的通信契约：

1. **连接**：`ws://localhost:<port>`（端口与 `--ws` 一致）。
2. **请求**（前端→Server）：标准 JSON-RPC 2.0 请求。
   ```json
   {"jsonrpc":"2.0","id":1,"method":"tools/call",
    "params":{"name":"sprite.decode",
              "arguments":{"archive":"DIABDAT.MPQ","name":"items\\helm.clx"}}}
   ```
3. **响应**（Server→前端）：
   ```json
   {"jsonrpc":"2.0","id":1,
    "result":{"name":"items\\helm.clx","format":"clx","width":64,"height":64,
              "frame_count":8,"palette_resolved":true,
              "frames":[{"index":0,"width":64,"height":64,
                         "rgba_base64":"AQIDBAUG..."}]}}
   ```
   `rgba_base64` 是**原始 RGBA8 字节**（非 PNG）的 base64，长度 = `width*height*4`。
   前端用 `atob` → `Uint8ClampedArray` → `new ImageData(arr,w,h)` 直接绘制（无需 PNG 解码，
   也便于做透明/调色板后处理）。若未来需要 PNG，可改取 `src/decoders/png.cpp::writePNG`
   的 base64（`data:image/png;base64,...`），但 RGBA 对查看器更灵活。
4. **错误**：`{"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"..."}}`。
5. **列举工具**：`{"method":"tools/list"}` → `result.tools[]`（含 name/description/schema）。
6. **枚举精灵（阶段 2 新工具 `sprite.list`）**：
   ```json
   {"jsonrpc":"2.0","id":2,"method":"tools/call",
    "params":{"name":"sprite.list","arguments":{"family":"plrgfx","class":"warrior"}}}
   // → result.sprites[] 含每件精灵的 name / width / type，前端据此建 职业×护甲×武器×动作 树
   ```
7. **自动取宽（阶段 2 增强 `sprite.decode`）**：
   ```json
   {"jsonrpc":"2.0","id":3,"method":"tools/call",
    "params":{"name":"sprite.decode",
              "arguments":{"archive":"DIABDAT.MPQ",
                           "name":"plrgfx\\warrior\\wlu\\wluas.cl2",
                           "width":"auto"}}}
   // width:"auto" 由 Server 按路径查 DevilutionX 数据表取宽，前端无需自带宽度表
   ```

> 契约与 stdio 完全一致 = 同一套 `dispatch`，前端无需特殊适配。rgba 为原始像素，
> 前端负责 ImageData 组装与 Canvas 绘制。

---

## 7. 依赖（Dependencies）

| 依赖 | 用途 | 引入方式 | 备注 |
|------|------|----------|------|
| **StormLib** | MPQ 读取/解密/解压 | vendored `3rdparty/StormLib`，`-DSTORM_USE_BUNDLED_LIBRARIES=ON` 自包含编译（zlib/bzip2/lzma/libtomcrypt/pklib 全打包） | 已验证可 MinGW-w64 编译为 `libStormLib.a` |
| **nlohmann/json** | JSON（tool I/O + JSON-RPC 分帧） | 单头文件 `json.hpp`（header-only，拖入 `3rdparty/nlohmann/`） | 无运行时依赖 |
| **winsock2 (ws2_32)** | WebSocket 桥的传输层 | Windows 系统库（链接 `ws2_32`） | 仅 Windows；WS 桥为自研最小 RFC6455 实现，未引入 mongoose |

> **WS 实现（已采用自研，无 mongoose 依赖）**：`src/mcp/transport_ws.cpp` 手写实现
> 了 RFC6455 的最小子集——HTTP `Upgrade` 握手（SHA-1 + base64 的
> `Sec-WebSocket-Accept`）、客户端掩码帧解码（text/ping/close/continuation）、
> 服务端文本帧发送，约 300 行纯 winsock 代码。完全 MIT 友好、零额外依赖，规避了
> mongoose 的 GPLv2/商用双授权顾虑。若未来需要更完整的 WS 特性（如扩展、TLS），可
> 替换 `transport_ws` 的实现而保持对外接口不变。

链接命令（MinGW-w64）：
```
-lStormLib -lws2_32 -lwininet
```
（`ws2_32` 供 WS 桥，`wininet` 供 StormLib 的远程归档支持。）

---

## 8. 构建（CMake + MinGW-w64）

`CMakeLists.txt` 要点：
- 生成器：`cmake -G "MinGW Makefiles"`（StormLib 已由仓库根 `build-stormlib.sh` 预编译为
  `3rdparty/StormLib/build/libStormLib.a`，CMake 直接链接该静态库）。
- 主目标 `mcp-server`：编译 `src/**` + 链接 `libStormLib.a` + `ws2_32` + `wininet`。
- `nlohmann/json` 以 include 目录（`3rdparty/`）方式加入，无需编译。
- **关键**：对可执行文件使用 `-static-libgcc -static-libstdc++`，避免运行时加载到
  PATH 上 Git 自带的不兼容 `libstdc++-6.dll` 而在字符串/容器操作时崩溃（已踩坑）。
- 可选：独立小目标 `mpq-cli`（仅 engine + mpq_tools，不含 MCP，便于单测）。

典型命令：
```bash
mkdir -p build && cd build
cmake -G "MinGW Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DSTORM_USE_BUNDLED_LIBRARIES=ON \
      ..
cmake --build . --target mcp-server
```

---

## 9. 落地顺序（实现计划）

按“先文档、后代码；先引擎、后工具、再 MCP”推进：

1. **文档** ✅：`docs/ARCHITECTURE.md`（本文）+ `PROJECT.md` 更新。
2. **脚手架** ✅：建 `src/{engine,decoders,tools,mcp}`，CMake 接入 StormLib +
   nlohmann/json（vendored 单头文件），链接静态运行时（`-static-libgcc
   -static-libstdc++`，避免加载到 Git 自带的不兼容 `libstdc++` 导致崩溃）。
3. **引擎封装** ✅：`src/engine` 包 StormLib（open/list/extract/verify/querySize），
   等价于 `tools/c/mpqextract` 但做成库函数；并对 stdin 传入的 POSIX 风格路径
   （`/d/...`）做 Windows 路径归一化。
4. **移植 mpq-inspect / mpq-recover 到 C** ✅：`src/tools/mpq_tools` 实现
   `mpq.list/extract/verify/recover`（recover 读字典→逐档案校验+取大小）。原
   `tools/*.ts` 已移至 `tools/ts-legacy/` 作对照。
5. **Tool Registry + JSON-RPC** ✅：`src/tools/registry` + `src/mcp/jsonrpc`，stdio
   传输已验证（`tools/list`、`tools/call` 全通）。
6. **WS 桥** ✅：`src/mcp/transport_ws` 采用**自研最小 RFC6455**（SHA-1 + base64 握手、
   掩码帧解码、文本帧发送，纯 winsock，零额外依赖，MIT 友好），已用 Node WebSocket
   客户端端到端验证。
7. **解码器（阶段 1，已完成）** ✅：`src/decoders/{endian_util,palette,clx,cel,cl2,pcx}`
   + `src/tools/sprite_tools` 全部落地，并注册 `palette.load` / `sprite.decode`（在
   `main.cpp` 调 `registerSpriteTools(reg)`）。调色板、CLX/CEL/CL2/PCX 解码器均为
   DevilutionX `load_*` 的忠实移植；CEL/CL2 通过 `celToClx`/`cl2ToClx` 转 CLX 后复用同一
   套 CL2 帧解码（透明 run / 不透明填充 / 不透明像素三种控制字节）。
   - **单元验证**（三套件，均 PASS，见 `README.md` §测试）：
     - `out/unit_tests.cpp`：CLX 解码、CEL→CLX / CL2→CLX 往返、PCX 内嵌调色板、调色板
       加载、base64 编解码（无需 MPQ）。
     - `out/core_tests.cpp`：`ToolRegistry` 注册/重名/分发/异常/`list`，`handleRequest`
       的 `tools/list`/`tools/call` 与各错误码（无需 MPQ）。
     - `out/integration_tests.cpp`：`engine::Archive` 全链路（open/list/verify/querySize/
       extract/extractToFile）+ `mpq.list`/`mpq.verify`/`mpq.extract`/`mpq.dump`/`mpq.recover`
       （需真实 MPQ，见 `DHFC_TEST_MPQ`；缺失则 SKIP）。
   - **端到端验证**：`palette.load` 对 `devilutionx.mpq` 的 `gendata\cut2.pal` 返回 256 色
     + 合法 RGBA base64；`sprite.decode` 对 `devilutionx.mpq` 的 `data\health.clx` +
     `ui_art\diablo.pal` 返回正确 264×19 RGBA 帧。
8. **地图（阶段 4，待实现）**：`src/decoders/level` + `map.build`。

> 第 2–6 步即用户选定的 “option B”：(1) 后端重构为 C tools/mcp 模块、
> (2) 移植 mpq-recover/mpq-inspect 到 C、(3) 脚手架 C MCP Server（stdio + WS 桥）。
> 现已全部完成并验证，后端彻底脱离 Node.js 运行时。

### 9.1 构建与运行

```bash
# 1) 先构建 StormLib（若尚未构建）
bash build-stormlib.sh

# 2) 构建 mcp-server
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -S . -B build-mcp
cmake --build build-mcp --target mcp-server

# 3) 运行
build-mcp/mcp-server.exe --stdio                 # 给 AI Agent
build-mcp/mcp-server.exe --ws 18765              # 给浏览器前端
build-mcp/mcp-server.exe --stdio --ws 18765      # 同时
```

---

## 10. 前端引用后端（阶段 2：角色 / 怪物查看器）

> 目标：浏览器里的**可视化查看器**，通过 WebSocket 调用本地 `mcp-server` 的
> `sprite.*` 工具拿到 RGBA 帧并绘制，自身**不读 MPQ、不碰文件系统**。这是把
> 阶段 1 的解码能力变成"可看"的下一步，也是阶段 2/3 查看器目标的天然载体。

### 10.1 当前已具备（无需重造）

- `mcp-server` 已带 **WS 桥**（`--ws <port>`，自研 RFC6455），与 stdio 共用同一套 tool `dispatch`。
- 后端已注册 `palette.load` 与 `sprite.decode`（`src/tools/sprite_tools.cpp`）：
  `sprite.decode` 能从 MPQ 抽出 `.clx/.cl2/.cel/.pcx` 并解码成
  `frames:[{index,width,height,rgba_base64}]`，前端用 `ImageData` 直接画。
- 因此"前端引用后端"的主干链路**已经打通**，缺的是让前端"好用"的少量后端补齐 + 前端壳。

### 10.2 后端需先补齐的 3 项（纯 C/C++，可独立验证）

1. **`sprite.decode` 支持 `width:"auto"`**（高优先）
   - 现 `sprite.decode` 对 `.cl2/.cel` 强制要求手填 `width`（`sprite_tools.cpp:169/175`）。
   - 复用 `mpq-info/render_sprite.cpp` 刚实现的路径→家族→宽度表查表逻辑
     （plrgfx→`sprites.tsv`、monsters→`monstdat.tsv`、missiles→`missile_sprites.tsv`、
     objects→`objdat.tsv`、towners→`towners.tsv`；items 无表仍手填）。
   - 实现位置：把 `autoWidth()` 抽成 `src/tools/sprite_tools.cpp` 内的静态函数（或
     提到 `src/decoders/` 共享），`width` 参数为 `"auto"` 或非正时调用。
2. **新增 `sprite.list` 工具**（高优先）
   - `mpq.list` 在无 listfile 时只给占位名 `FileNNNNNNN.xxx`，前端无法据此浏览。
   - `sprite.list` 改读 `mpq-info/dict/recovered.json` + DevilutionX 数据表，枚举
     **真实存在**的精灵名，并按家族/职业/动作归类，返回
     `{sprites:[{name,archive,family,class,type,width,frames?}]}`。
   - 前端据此建"职业 × 护甲 × 武器 × 动作"目录树，点击即 `sprite.decode`。
3. **（可选，保真度）TRN 上色**
   - `sprite.decode` 现只解析基Palette，未应用职业 TRN（`plrgfx/<class>.trn`，`GetClassTRN`）
     与每精灵 TRN（`GetPlayerGraphicTRN`）。
   - 可加参数 `apply_trn:true`，从 MPQ 取对应 `.trn`（256 字节色彩变换表）后作用于帧
     再返回。第一版前端可先只用 `diablo.pal` 跑通链路，TRN 作为保真增强后续补。

### 10.3 前端目录结构（TS + Canvas，仅前端构建，无 Node 运行服务）

```
web/                              # 浏览器前端（不读 MPQ/文件，仅 WebSocket 调后端）
├── index.html                    # 入口：<canvas> + 目录树 + 方向/帧控制
├── src/
│   ├── client.ts                 # WS 客户端：tools/list / tools/call，id 配对并发
│   ├── viewer.ts                 # Canvas 绘制：atob→ImageData→putImageData
│   └── catalog.ts                # 用 sprite.list 建 职业×护甲×武器×动作 树
├── dist/                        # tsc 编译产物（静态 JS，可直接 file:// 或 http 打开）
└── build.sh                     # tsc --outDir dist（仅前端构建，产生的仍是静态文件）
```

- 前端**不引入 Node 作为运行服务**；`dist/` 是纯静态文件，用任意静态服务器
  （`python -m http.server` 等）或本地 `file://` 打开，连 `ws://localhost:18765`。
- 绘制用 Canvas 2D `ImageData`（rgba_base64 即原始像素，最灵活）；如后续要 3D/滤镜
  再考虑 WebGL，但不在阶段 2 范围。

### 10.4 工作顺序（建议）

1. 后端：`sprite.decode` 加 `width:"auto"`（复用 render_sprite 查表）→ 跑三套件测试。
2. 后端：新增 `sprite.list`（读 recovered.json + 数据表）→ 跑三套件测试。
3. 后端（可选）：`sprite.decode` 加 `apply_trn` 上色。
4. 前端：脚手架 `web/`，`client.ts` 连 WS → `tools/list` → `sprite.list` 取目录 →
   `sprite.decode` 取 RGBA → `viewer.ts` 绘制；先做"选精灵 → 看单帧"。
5. 查看器能力叠加（阶段 2/3 目标）：
   - 8 方向切换（玩家固定 8 方向，按 `帧数/8` 切分；怪物方向数另查动画表）。
   - 装备组合浏览（前缀 `c+a+w` 枚举：D1 武器/护甲已 bake 进文件名，非独立图层，
     直接枚举前缀即可，无需通用合成器）。
   - 技能特效叠加（`lm`/`fm`/`qm` 等特效精灵本就在 plrgfx 前缀枚举内）。
   - 怪物/NPC 查看器（阶段 3）复用同一 `sprite.list` + `sprite.decode` 链路，仅家族不同。

> 该顺序让"列清单 → 渲染 → 显示"尽快端到端跑通，且后端每一步都可独立单测，
> 符合项目"先理解、再使用；CLI / 可验证产出优先"的章程。
