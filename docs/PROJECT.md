# Diablo 1（Hellfire 版本）MPQ 资源精研与查看器 — 项目定位文档

> 本文档记录项目的定位、范围、资源来源、技术路线与分阶段计划。
> 它是项目的“宪章”，后续所有实现任务都应服务于这里定义的目标。

---

## 1. 项目使命（Mission）

精研 **Diablo 1 的 Hellfire 扩展版本** 所使用的 MPQ 资源包，逐一确认其中
**每个文件、每种文件类型的具体作用**；并以“任务导向”的方式，利用这些资源完成
四个可交付目标（见第 2 节）。

核心理念：

- **先理解，再使用**：不盲目解析，而是先建立对每种资源格式的完整认知。
- **以权威实现为参考**：`D:\work\xt\diablo\DevilutionX` 是已确认能正确读取这些
  精确 MPQ 的开源实现，其解码逻辑是本项目的最高参考。

  > **架构决策（2026-11-06 初定，2026-11-07 演进确认）**：
  > - *底层* MPQ 容器与编解码（哈希表/块表解密、`DecryptMpqBlock`、逐文件解密
  >   key=`HashString(name,3)+offset`、PKWARE implode、Blizzard 多压缩分派、扇区
  >   偏移表等）**直接采用参考实现的 C 源码经 MinGW-w64 编译为本地引擎**（基于
  >   StormLib，即 DevilutionX `3rdParty/mpqfs` 的上游），**不手写移植到
  >   TypeScript**——既脆弱又重复造轮子（已验证可用）。
  > - *2026-11-07 演进*：经用户确认，**后端整体转向纯 C/C++，彻底脱离 Node.js 运行时**。
  >   所有 CLI/工具链将重构为一个本地 **MCP Server**（C/C++ 实现），同时提供
  >   **stdio 传输**（给 AI Agent）与 **WebSocket 桥接**（给未来的 TS/Canvas/WebGL
  >   浏览器前端）。TypeScript **仅保留给前端**（绘制层），不再承担后端逻辑；
  >   现有 `tools/*.ts` 的 MPQ 逻辑将**移植到 C**（见 `docs/ARCHITECTURE.md`）。
  >   详细模块布局、传输设计与 WS 契约见 **`docs/ARCHITECTURE.md`**。
- **CLI 优先、避免 GUI**：基础工具链以命令行为主；查看器类的产出可输出为
  图像/数据/文本，不急于搭建图形界面。

---

## 2. 范围与目标（交付物）

项目最终要交付四个以 MPQ 资源为基础的查看/还原工具：

1. **还原完整的游戏地图**
   - 解析地牢/城镇的 TIL（瓦片数据）、MIN（关卡片段）、DUN（布局）等文件，
     重建每一层的完整地图与寻路/光照信息。
   - 输出：可浏览的关卡地图（静态渲染 + 可选的瓦片坐标系）。

2. **完整的角色查看器（Character Viewer）**
   - 覆盖所有职业（Warrior / Rogue / Sorcerer，以及 Hellfire 的 Monk）。
   - 展示所有 **装备外观**（武器、盔甲、头盔、盾牌等图层叠加）。
   - 展示所有可用 **技能的动作、特效叠加**（闪电、火、冰等效果层）。
   - 展示所有 **可用动作**（行走、攻击、施法、受击、死亡、空闲等动画模式）。

3. **完整的 Monster / NPC 查看器**
   - 包含所有怪物与 NPC 的精灵与动画。
   - 展示其所有可用 **技能的动作、特效叠加、所有可用动作**（同角色查看器）。

4. **完整的界面 GUI 布局与显示**
   - 还原游戏全部界面布局：主菜单、角色面板、物品栏、技能树、任务日志、
     血/蓝球、商店、对话框等（参考 `DiabloUI/` 目录）。

> 注：上述 2/3/4 的“查看器”在实现阶段是否以图形界面呈现，留待后续决定；
> 当前阶段优先产出 **可验证的数据与离屏渲染结果**（PNG/JSON/文本），
> 确保每种资源都被正确解码。

---

## 3. 资源来源与参考

### 3.1 原始 MPQ 资源包位置

```
D:\work\xt\diablo\DevilutionX-Hell Fire\
```

已确认存在的 MPQ（早期由已退役的 TS 探查工具解析，见第 4 节真实统计；现由
`mcp-server` 的 `mpq.list` 直接读取）：

| 文件 | 说明 |
|------|------|
| `DIABDAT.MPQ` | 原版 Diablo 1 主体资源（最大，约 517 MB） |
| `hellfire.mpq` | Hellfire 扩展主体资源 |
| `hfmonk.mpq` | Hellfire 武僧（Monk）职业资源 |
| `hfvoice.mpq` | Hellfire 语音 |
| `hfopt1.mpq` / `hfopt2.mpq` | Hellfire 可选内容/补丁包 |
| `hfmusic.mpq` | Hellfire 音乐（未在首批统计内，待补充） |
| `devilutionx.mpq` | DevilutionX 引擎自有资源（UI、字体覆盖等） |
| `fonts.mpq` | 字体资源 |
| `spawn.mpq` | Spawn 版本资源 |
| `es.mpq` / `pl.mpq` / `ru.mpq` | 多语言本地化包 |

### 3.2 权威参考实现

```
D:\work\xt\diablo\DevilutionX\
```

这是确认能读取上述 Hellfire MPQ 的实现。**关键参考点**（已初步探查）：

- MPQ 读取：`Source/mpq/mpq_reader.cpp` → 实际解码委托给 `3rdParty/mpqfs/`
  （StormLib 风格的 MPQ 库，git submodule，本机未检出源码，但行为以 StormLib
  为准）。本项目自行实现 MPQ 解析时，以 StormLib 的 `DecryptMpqBlock` 算法为基准。
- 精灵/动画解码：`Source/engine/load_cel.cpp`、`load_cl2.cpp`、`load_clx.cpp`、
  `load_pcx.cpp`、`palette.cpp`、`animationinfo.cpp`。
  - CEL / CL2 是 Diablo 1 的主要精灵/动画格式；DevilutionX 在加载时把 CL2 转换为
    内部 CLX 格式（`cl2_to_clx`）。
- 关卡/地图：`Source/levels/`（`drlg_l1..l4`、`gendung`、`town`、`themes`、
  `dun_tile_data`、`tile_properties` 等）。
- 数据表：`Source/tables/`（`monstdat` 怪物、`spelldat` 技能、`playerdat` 角色、
  `itemdat` 物品、`misdat` 导弹/特效、`objdat` 物件等），运行时从 MPQ 内
  `data\*.txt` 加载。
- 界面：`Source/DiabloUI/`（主菜单、角色选择、按钮、滚动条、对话框等）。

> 已踩坑验证：Diablo/Hellfire 的 MPQ **哈希表与块表是加密存储的**，必须用
> StormLib 的 `DecryptMpqBlock` 算法（先混合 seed，再 XOR `(key+seed)`，随后对 key
> 做 `(~key<<0x15)+0x11111111 | (key>>0x0B)` 置换、用解密结果演化 seed）才能正确
> 解出块表；用简化版解密会得到全随机数据。此逻辑现位于 `src/tools/mpq_tools.cpp`
> （由 `mcp-server` 的 `mpq.*` 工具暴露），早期曾写在已退役的 `tools/mpq-inspect.ts`。

---

## 4. MPQ 资源清单（真实统计）

（早期由已退役的 `tools/mpq-inspect.ts` 解析得到；现可由 `mcp-server` 的
`mpq.list` / `mpq.dump` 复现；format version 均为 0，sector size 4096）：

| MPQ | 大小 | 文件数(exists) | 加密文件 | 未压缩合计 | 压缩合计 | 压缩率 |
|-----|------|---------------|----------|-----------|----------|--------|
| DIABDAT.MPQ | 517,501,258 | **2,910** | 2,900 | 572,465,074 | 517,346,918 | 90.4% |
| hellfire.mpq | 65,502,336 | **584** | 582 | 81,314,978 | 65,459,712 | 80.5% |
| hfmonk.mpq | 37,658,368 | **336** | 336 | 51,351,739 | 37,636,096 | 73.3% |
| hfvoice.mpq | 37,743,520 | **90** | 90 | 41,194,848 | 37,737,472 | 91.6% |
| hfopt1.mpq | 687,274 | 13 | 1 | 1,350,784 | 669,725 | 49.6% |
| hfopt2.mpq | 1,092,396 | 60 | 1 | 2,068,151 | 1,062,638 | 51.4% |
| devilutionx.mpq | 5,562,231 | 143 | 1 | 11,176,393 | 5,555,815 | 49.7% |
| fonts.mpq | 58,595,557 | 770 | 1 | 116,563,978 | 58,566,821 | 50.2% |
| spawn.mpq | 25,448,219 | 1,030 | 2 | 40,971,424 | 25,398,939 | 62.0% |
| hfmusic.mpq | 34,379,360 | 2 | 2 | 34,834,940 | 34,378,752 | 98.7% |
| es.mpq | — | 723 | — | — | — | — |
| pl.mpq | — | 517 | — | — | — | — |
| ru.mpq | — | 749 | — | — | — | — |

> 说明：原版游戏 MPQ **不带 (listfile)**，因此“文件总数”来自解密后的块表
> EXISTS 标志位；单个文件名需借助 DevilutionX 的已知文件名清单或后续建立的文件名
> 推导来确认（这是“确认每个文件作用”研究阶段的核心工作）。

---

## 5. 文件类型与用途（格式词典 / 待确认清单）

> 下列为 Diablo 1 / Hellfire 已知格式的初始假设。项目研究阶段将逐一验证每个
> 文件在 MPQ 中的真实角色。

| 扩展名 | 类型 | 用途（假设，待确认） |
|--------|------|----------------------|
| `.cel` | 精灵/动画（调色板索引 + RLE） | 角色、怪物、物品、瓦片、特效等绝大多数精灵 |
| `.cl2` | 编译型 CEL（带动画帧组） | 角色/怪物动画、特效叠加层；DevilutionX 加载时转 CLX |
| `.clx` | DevilutionX 内部精灵格式 | 由 CEL/CL2 转换而来，含帧偏移头 |
| `.pcx` | 静态图像（PC Paintbrush） | 部分静态图（参考 `load_pcx.cpp`） |
| `.pal` | 调色板（256 色） | 精灵上色所需的颜色查找表 |
| `.til` | 瓦片数据 | 每张瓦片的 4 个角信息，用于关卡拼接（`dun_tile_data`） |
| `.min` | 迷你瓦片 / 关卡片段 | 与 TIL 配合构建地牢基本单元 |
| `.dun` | 地牢布局 | 指定层内瓦片/片段的排布（`drlg_*`、`gendung`） |
| `.sol` | （待确认） | 待研究确认 |
| `.bin` | 二进制数据 | 字体、光标、关卡索引等各类二进制 |
| `.txt` | 数据表 | `data\Monsters.txt`、`Spells.txt`、`Items.txt`、
  `UniqueItems.txt`、`CharStats.txt` 等（`Source/tables/` 加载） |
| `.smk` | Smacker 视频 | 开场/过场动画 |
| `.wav` | 音效/语音 | 战斗音效、NPC 语音（`hfvoice.mpq`） |
| `.mid` / `.mus` | MIDI 音乐 | 背景音乐（`hfmusic.mpq`） |

---

## 6. 技术路线与工具链

### 6.1 语言与运行环境（已与用户确认，2026-11-07 演进）

> 详见 `docs/ARCHITECTURE.md`。后端与 MCP Server 纯 C/C++，**无 Node.js 运行时**。

- **C/C++（后端主语言，已确认）**：MinGW-w64 gcc/g++ 16.2
  （`D:\work\xt\toolchains\mingw64`）。MPQ 引擎（StormLib 封装）、所有解码器、
  工具注册表、MCP Server（stdio + WebSocket 桥）均用 C/C++ 实现，编译为单一原生
  可执行 `mcp-server.exe`。
- **TypeScript（仅前端）**：未来的浏览器查看器（Canvas/WebGL 绘制），通过
  WebSocket 与本地 `mcp-server` 通信，**不读 MPQ、不碰文件系统**。Node 不再用于
  后端工具链。
- **Go 1.27**（`D:\develop\go`）：备选，当前不需要。
- **纯 TS 脚本的现状**：`tools/mpq-inspect.ts`、`tools/mpq-recover.ts` 是阶段 0 的
  **参考/验证件**，其逻辑将移植到 C（`src/tools/mpq_tools`），不再是主路径。

### 6.2 工具原则

- 命令行优先，避免 GUI（前端除外，前端是 Web）。
- **后端纯 C/C++，无 Node 运行时**：底层 MPQ/编解码用参考实现的 C 源码（StormLib）
  经 MinGW-w64 编译；上层工具、解码器、MCP Server 也用 C/C++（见第 1 节与
  `docs/ARCHITECTURE.md`）。**不手写移植到 TypeScript**。
- 所有能力以 **MCP 工具（tool）** 形式暴露：每个工具是“具名 + 参数 + 返回 JSON”
  的 C 函数，注册到 Tool Registry，由 stdio 与 WebSocket 两种传输复用。
- 所有解析以“可验证产出”为目标：工具返回 JSON（含 base64 图像 / 结构元数据 /
  文本清单），便于 Agent 与前端核对每类资源是否正确解码。

### 6.3 已有基础工具

**C / StormLib 引擎（底层，已验证可用）**
- `3rdparty/StormLib/`：vendored 的 StormLib（DevilutionX `mpqfs` 的上游，Diablo 1
  兼容），用 `-DSTORM_USE_BUNDLED_LIBRARIES=ON` 自包含编译（无需外部依赖），产物
  `3rdparty/StormLib/build/libStormLib.a`。
- `build-stormlib.sh`（仓库根）：仅构建上述 StormLib 静态库（StormLib 以 git submodule vendored
  于 `3rdparty/StormLib`；不再构建任何独立 CLI —— 旧的
  `mpqextract` 独立工具已退役，其能力全部并入 `src/` 后端的 `mpq.*` 工具）。

**目标后端（`src/`，已落地并验证，详见 `docs/ARCHITECTURE.md`）**
- `src/engine/`：StormLib 封装（`open/list/extract/verify/querySize`），库函数形态。
- `src/decoders/`：CEL/CL2/CLX/PCX/PAL/TIL/MIN/DUN 解码（阶段 1+ 待实现；已建目录）。
- `src/tools/`：Tool Registry + `mpq_tools`（已落地
  `mpq.list`/`mpq.extract`/`mpq.verify`/`mpq.recover`/`mpq.dump`；其中 `mpq.recover`
  即原 `mpq-recover.ts` 的 C 移植，`mpq.dump` 是原 `mpqextract dump` 子命令的移植）。
- `src/mcp/`：MCP Server，`jsonrpc` + `transport_stdio` + `transport_ws`（自研最小
  RFC6455，无 mongoose 依赖）。
- 产物：`mcp-server.exe`（单一原生可执行，无 Node 运行时，已构建并验证）。

**旧版 TypeScript 编排层（已删除）**
- 原 `tools/mpq-inspect.ts` / `tools/mpq-recover.ts` 及其临时归档 `tools/ts-legacy/`
  已随“TS→C 迁移 + 目录重构”一并移除；其全部逻辑均已落在 `src/` 后端，无遗留参考件。
  （MPQ 文件名恢复用的参考资料现集中于 `mpq-info/`，见下。）
- `mpq-info/dict/`：候选名字典（`diablo1-harvest.txt` 等）与恢复产出（`recovered/`、
  `recovered.json`）。`recovered.json` 现在可由 `mcp-server` 的 `mpq.recover` 工具
  按档案重建。

---

## 7. 目录结构规划（建议）

```
DiabloHFConverter/
├── 3rdparty/
│   ├── StormLib/        # vendored StormLib（C MPQ 引擎上游，已编译为静态库）
│   │   └── build.sh     # 仅构建该静态库（供 mcp-server 链接）
│   └── nlohmann/        # nlohmann/json.hpp（header-only，已引入）
├── docs/
│   ├── PROJECT.md       # 项目定位文档（本文件）
│   └── ARCHITECTURE.md  # 后端 / MCP 架构（stdio + WS 桥，Node-free C/C++）
├── src/                 # 后端（C/C++，已落地并验证）
│   ├── engine/          # StormLib 封装（open/list/extract/verify/querySize）
│   ├── decoders/        # CEL/CL2/CLX/PCX/PAL/TIL/MIN/DUN 解码（阶段 1+ 待实现）
│   ├── tools/           # Tool Registry + mpq_tools（mpq.* 工具）
│   ├── mcp/             # MCP Server：jsonrpc / stdio / ws 桥
│   └── main.cpp         # 入口（--stdio / --ws <port>）
├── mpq-info/            # MPQ 文件名恢复参考/产出（非代码，仅数据）
│   ├── _raw_paths.txt   # 原始路径/名线索
│   └── dict/            # 候选名字典与恢复产出
│       ├── diablo1-harvest.txt   # 从 DevilutionX 源 harvest 的候选名
│       ├── recovered/           # 各档案已恢复名（name<TAB>size）
│       └── recovered.json       # 汇总清单（可由 mpq.recover 重建）
├── build-mcp/           # CMake 构建目录（mcp-server.exe 产物）
├── CMakeLists.txt       # MinGW 构建（StormLib + 静态运行时 + ws2_32/wininet）
└── out/                 # 产出的 PNG/JSON/文本（运行产物，不入库）
```

---

## 8. 分阶段任务计划

### 阶段 0：基础 — MPQ 访问（基本完成）
- [x] 实现 MPQ 头/哈希表/块表解析 + 正确解密（已验证 DIABDAT 2910 文件）。
- [x] **单文件提取**：底层引擎采用 StormLib（C）经 MinGW-w64 编译为
      `3rdparty/StormLib/build/libStormLib.a`，由 `src/engine` 封装，并暴露为
      `mcp-server` 的 `mpq.extract` / `mpq.dump` 工具，已验证可正确完成
      「哈希表查找 → 逐文件解密 (key=`HashString(name,3)+offset`) → PKWARE implode
      / 多压缩解压」全链路（抽出的 `gendata\cut2.pal` = 768 字节 = 256×3 调色板；
      `nlevels\l5data\l5.cel` = 1.3 MB 等均正确）。
- [x] **文件名恢复**：机制已完成并验证——以 DevilutionX 源 harvest 的候选名 + 少量
      手工补名组成字典，调用 C 引擎逐档案校验（成功=名真实存在），产出
      `mpq-info/dict/recovered.json` / `recovered/*.txt`。
  - **覆盖率提升路线（已完成，2026-11-07）**：Diablo 1 大量文件名是运行时动态拼出来的
    （plrgfx / monsters / missiles / items / objects / towners），不在静态字面量中。
    已在 DevilutionX 源码定位到拼接公式，并实现为代码生成器
    **`mpq-info/recover_phase0.cpp`**：它读取 DevilutionX 权威数据表
    （`assets/txtdata/classes/*/sprites.tsv`、`monstdat.tsv`、`missile_sprites.tsv` 及
    Hellfire 的 `mods/hf/txtdata/...`），依据下列公式批量生成候选名，再与静态字典
    `diablo1-harvest.txt` 合并，逐档案校验并重建 `recovered.json`：
    - `player.cpp:1506` → `plrgfx\{classPath}\{c}{a}{w}\{c}{a}{w}{type}.cl2`
      （`c`=职业首字母，`a`∈{l,m,h} 护甲，`w`∈{n,u,s,d,b,a,m,h,t} 武器，
      `type`∈{as,st,aw,wl,at,ht,lm,fm,qm,dt,bl} 共 11 种动作；已确认原版精灵存为 **.cl2**）
    - `monster.cpp:3221` + `Animletter="nwahds"` → `monsters\{assetsSuffix}{L}.cl2`
      （`assetsSuffix` 来自 `monstdat.tsv`，`L`∈{n,w,a,h,d,s}，另含 `monsters\{trnFile}.trn`）
    - `tables/misdat.cpp:420` → `missiles\{name}.cl2`（精灵基名取自 `missile_sprites.tsv`
      的 `name` 列，非 `misdat` 的法术名）
    - `items.cpp:2428` → `items\{ItemDropNames[i]}.cel`（掉落/背包精灵，由 `LoadCel` 加载，
      `ItemDropNames` 为 `items.cpp:230` 的 44 项固定游戏常量；另含 `items\duricons.cel`、
      `items\map\mapztown.cel` 两个 HUD 静态精灵；已确认存为 **.cel**）
    - `objects.cpp:3720` → `objects\{ObjMasterLoadList[i]}.cel`（地牢场景/互动物件，由
      `LoadCelWithStatus` 加载；`ObjMasterLoadList` 取自 `objdat.tsv` 的 `file` 列，去重后
      共 62 个；已确认存为 **.cel**）
    - `towners.cpp:130` → `towners\{...}.cel`（城镇 NPC 精灵，由 `LoadTownerAnimations`
      经 `LoadCel` 加载；路径取自 `towners.tsv` 的 `animPath` 列，另含 `farmer\cfrmrn2` /
      `farmer\mfrmrn2` / `girl\girls1` / `animals\cow` 等硬编码变体；已确认存为 **.cel**）
  - **效果**：真实文件名恢复数由静态字典的 **196** 提升至 **2,080**
    （DIABDAT 1224 / hfmonk 291 / spawn 315 / hellfire 171 / devilutionx 63 / …）。
    构建与运行见仓库根 `README.md` §测试 第 4 项。
  - 注：无 listfile 时 `list` 仅能给占位名（`FileNNNNNNN.xxx`），故文件名恢复依赖
    上述字典校验法；一旦覆盖率足够高，亦可把这些名作为 listfile 喂给 StormLib。
  - **剩余空白（后续可选）**：plrgfx / monsters / missiles / items / objects / towners
    等运行时拼接名均已纳入候选生成。仍可继续扩展其它拼接族（如 `missiles` 的附加帧、
    各 `TRN` 调色板表、字体 `fonts\*.trn` 等）以提升覆盖率。

> **后端重构（已完成，2026-11-07）**：原 TS 编排层（`mpq-inspect.ts` /
> `mpq-recover.ts`）已完整移植到 C/C++，目录已重构为 `src/` 后端 + `tools/ts-legacy/`
> 参考件。现有一个纯 C/C++、无 Node 运行时的 `mcp-server.exe`，同时提供
> **stdio**（给 AI Agent）与 **WebSocket 桥**（给未来 TS/Canvas/WebGL 前端）两种传输，
> 复用同一套 Tool Registry。已验证工具：`mpq.list` / `mpq.extract` / `mpq.verify` /
> `mpq.recover`（DIABDAT 静态字典已恢复 81 个真实文件名）。详见
> `docs/ARCHITECTURE.md`。阶段 1+ 的解码器（`src/decoders/`）待实现。

### 阶段 1：精灵与调色板解码
- [ ] 解码 `.pal` 调色板。
- [ ] 解码 `.cel` / `.cl2`（含帧组、RLE、透明），移植 `load_cel` / `load_cl2`。
- [ ] PCX 解码（`load_pcx`）。
- [ ] 产出：任意精灵 → PNG 的离屏渲染。

### 阶段 2：角色查看器（目标 2）
- [ ] 枚举各职业所有动画模式（walk/attack/spellcast/hit/death/idle…）。
- [ ] 装备图层叠加（武器/盔甲/头盔/盾牌）。
- [ ] 技能特效叠加层渲染。
- [ ] 产出：按“职业 × 动作 × 装备 × 技能特效”组合的查看数据。

### 阶段 3：怪物 / NPC 查看器（目标 3）
- [ ] 枚举怪物/NPC 精灵与动画模式。
- [ ] 怪物技能动作与特效叠加。
- [ ] 产出：怪物/NPC 查看数据。

### 阶段 4：地图还原（目标 1）
- [ ] 解码 TIL / MIN / DUN，移植 `levels/` 的拼接逻辑。
- [ ] 重建各层完整地图（瓦片、光照、碰撞）。

### 阶段 5：GUI 布局与显示（目标 4）
- [ ] 还原 `DiabloUI/` 的全部界面布局与元素。
- [ ] 输出界面布局数据与可视化。

> 每个阶段都以“确认该类资源被正确解码”为完成标准，优先产出可验证的
> 数据/图像，再决定是否做图形界面。

---

## 9. 关键参考位置速查（DevilutionX）

| 关注点 | 路径 |
|--------|------|
| MPQ 读取封装 | `Source/mpq/mpq_reader.cpp` |
| 底层 MPQ 解码库 | `3rdParty/mpqfs/`（StormLib 风格；行为以 StormLib `DecryptMpqBlock` 为准） |
| CEL 加载 | `Source/engine/load_cel.cpp` |
| CL2 加载 | `Source/engine/load_cl2.cpp` |
| CLX 加载 | `Source/engine/load_clx.cpp` |
| CL2→CLX 转换 | `Source/engine/utils/cl2_to_clx.hpp` |
| PCX 加载 | `Source/engine/load_pcx.cpp` |
| 调色板 | `Source/engine/palette.cpp` |
| 动画信息 | `Source/engine/animationinfo.cpp` |
| 关卡生成 | `Source/levels/`（`drlg_l1..l4`、`gendung`、`town`、`themes`、`dun_tile_data`） |
| 怪物数据 | `Source/tables/monstdat.cpp`、`Source/monster.cpp`、`Source/monsters/` |
| 技能数据 | `Source/tables/spelldat.cpp`、`Source/spells.cpp` |
| 物品数据 | `Source/tables/itemdat.cpp`、`Source/items.cpp` |
| 导弹/特效 | `Source/missiles.cpp` |
| 界面 | `Source/DiabloUI/` |

---

## 10. 备注：Hellfire 版本特殊性

- 本项目的 MPQ 是 **Hellfire（不是原版 Diablo 1 单独）**，因此包含额外资源：
  `hellfire.mpq`、`hfmonk.mpq`（武僧）、`hfvoice.mpq`、`hfopt1/2.mpq`、
  `hfmusic.mpq`。
- 解码逻辑必须兼容这些 MPQ 的精确格式；以 DevilutionX 的成功读取为正确性准绳。
- 多语言包（`es/pl/ru.mpq`）主要用于文本本地化，非本项目核心，但可作为
  字符串来源参考。
