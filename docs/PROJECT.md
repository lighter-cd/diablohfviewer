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
    - **规则文档**：上述六大家族（plrgfx / monsters / missiles / items / objects / towners）
      及 TRN 调色板的完整命名公式、DevilutionX 数据源、已验证扩展名与 `recover_phase0.cpp`
      函数映射，见 **`docs/RUNTIME_FILENAME_RULES.md`**。
    - `towners.cpp:130` → `towners\{...}.cel`（城镇 NPC 精灵，由 `LoadTownerAnimations`
      经 `LoadCel` 加载；路径取自 `towners.tsv` 的 `animPath` 列，另含 `farmer\cfrmrn2` /
      `farmer\mfrmrn2` / `girl\girls1` / `animals\cow` 等硬编码变体；已确认存为 **.cel**）
    - **TRN 调色板（256 字节色彩变换表）**：
      - `fonts\{name}.trn`：`text_render.cpp` 中的固定字体调色板清单（black/white/red/…）。
      - `plrgfx\{sprites.tsv 的 trn 字段}.trn`：职业调色板（`GetClassTRN`），另含字面量
        `plrgfx\infra.trn` / `plrgfx\stone.trn` / `gendata\pause.trn`（`lighting.cpp`/`trn.cpp`）。
      - 每个玩家精灵的 `plrgfx\{class}\{prefix}\{prefix}{type}.trn` 变体
        （`GetPlayerGraphicTRN` 构造 `pszName + ".trn"`）。
      - 怪物色彩 TRN `monsters\{trnFile}.trn` 已由上列 `monsters` 公式经 `trnFile` 列覆盖
        （如 `monsters\acid\acidb.trn`、`monsters\bat\grey.trn` 等，共 ~94 个）。
  - **效果**：真实文件名恢复数由静态字典的 **196** 提升至 **2,080**
    （DIABDAT 1224 / hfmonk 291 / spawn 315 / hellfire 171 / devilutionx 63 / …）。
    其中 TRN 调色板已恢复 **111** 个（跨档案行数；唯一名 **85**；DIABDAT 68 + spawn 26 +
    devilutionx 17 + …）。
    构建与运行见仓库根 `README.md` §测试 第 4 项。
  - 注：无 listfile 时 `list` 仅能给占位名（`FileNNNNNNN.xxx`），故文件名恢复依赖
    上述字典校验法；一旦覆盖率足够高，亦可把这些名作为 listfile 喂给 StormLib。
  - **进度评估（是否需达 100%）**：**不需要**。阶段 0 的完成标准是“运行时拼接名
    被公式化候选生成并逐一校验”，而非 100% 总文件覆盖。当前总恢复 **2,080 / 7,927 ≈ 26.2%**，
    但剩余 ~73% 主要是**不受拼接公式约束**的资源（关卡几何、视频、音频、字体、多语言、
    音乐），本就不在阶段 0 范围内；对六大家族 + TRN，候选生成已穷尽公式所能产出的全部
    组合，凡确实存在于 MPQ 中的名均已恢复（生成器已无新的可校验候选）。

  - **各标准项当前进度**：

    | 标准项 | 状态 | 已恢复（唯一名 / 跨档案行数） | 说明 |
    |--------|------|------------------------------|------|
    | MPQ 头/哈希表/块表解析 + 解密 | ✅ 完成 | DIABDAT 2910 文件可解析 | 已验证 |
    | 单文件提取（解密→解压→落盘） | ✅ 完成 | `cut2.pal`(768B)、`l5.cel`(1.3MB) 正确 | 已验证 |
    | `plrgfx`（玩家精灵） | ✅ 公式全覆盖 | **1058** 唯一名 | `player.cpp:1506` 组合穷尽 |
    | `monsters`（怪物精灵 + 色彩TRN） | ✅ 公式全覆盖 | **376** 唯一名 | `monster.cpp` + `trnFile` 列 |
    | `missiles`（弹道精灵） | ✅ 公式全覆盖 | **33** 唯一名 | `misdat.cpp:420` |
    | `items`（掉落/HUD 精灵） | ✅ 公式全覆盖 | **45** 唯一名 | `items.cpp:230` 44 项 + 2 HUD |
    | `objects`（场景物件） | ✅ 公式全覆盖 | **62** 唯一名 | `objdat.tsv` `file` 列 |
    | `towners`（NPC 精灵） | ✅ 公式全覆盖 | **15** 唯一名 | `towners.tsv` `animPath` + 4 硬编码 |
    | TRN 调色板（字体/职业/怪物色彩） | ✅ 公式全覆盖 | **85** 唯一名 / ~111 行 | `fonts\*.trn`(16)+`monsters\*.trn`(~66)+`plrgfx\infra/stone`(2)+`gendata\pause`(1) |
    | 静态字典补充（关卡/视频/音频等） | ✅ 完成 | 余下名（.dun 52/.pal 27/.smk 11/.wav 10/.cel 228…） | `diablo1-harvest.txt` |
    | 后端重构（TS→C/C++ mcp-server） | ✅ 完成 | stdio + WS 双传输 | — |

    > 逐档案 canonical 计数（`safeName` 修复前残留的 `*.mpq.txt` 重复文件已清理，现仅余 13 个 canonical `*.txt`）：
    > DIABDAT **1224** / spawn **315** / hfmonk **291** / hellfire **171** / devilutionx **63** /
    > hfopt1 9 / hfopt2 2 / hfmusic 2 / hfvoice 1 / pl 1 / ru 1 / **fonts 0** / **es 0**；
    > 合计 **2080**，唯一名 **1750**。
    > 注：玩家逐精灵 `plrgfx\{class}\{prefix}\{prefix}{type}.trn` 候选经校验命中为 **0**，
    > 仅职业级 `infra.trn`/`stone.trn` 真实存在（玩家着色走单一职业 TRN + 运行时），生成器
    > “测过并正确排除”，符合“无假阳性”机制。

  - **后续建议（非强制）**：
    1. 阶段 0 可正式标记“完成”，无需追 100%。
    2. 可选补充极个别拼接族（优先级极低）：唯一怪物 `monsters\monsters\{trnName}.trn`
       （该数据集 `trn` 列为空）、DiabloUI 调色板；补上也仅个位数增量。
    3. 数据整洁性：清理 `recovered/` 下 `safeName` 修复前的 `*.mpq.txt` 残留（如
       `spawn.mpq.txt`=216 行 vs canonical `spawn.txt`=315 行），避免双倍计数误导。
    4. 若未来要提升“总文件覆盖率”（非阶段 0 目标），方向是关卡/视频/音频/字体/多语言，
       需静态字典或暴力枚举，建议留到对应阶段（如字体、视频在阶段 1+）。`fonts.mpq`(0/770)、
       `es.mpq`(0/723) 的 0 命中正说明其不在公式覆盖族内。
    5. 下一真正有价值工作为**阶段 1**：精灵/调色板解码（`.pal`/`.cel`/`.cl2` → PNG 离屏渲染），
       那才是“确认资源被正确解码”的验证点。

> **后端重构（已完成，2026-11-07）**：原 TS 编排层（`mpq-inspect.ts` /
> `mpq-recover.ts`）已完整移植到 C/C++，目录已重构为 `src/` 后端 + `tools/ts-legacy/`
> 参考件。现有一个纯 C/C++、无 Node 运行时的 `mcp-server.exe`，同时提供
> **stdio**（给 AI Agent）与 **WebSocket 桥**（给未来 TS/Canvas/WebGL 前端）两种传输，
> 复用同一套 Tool Registry。已验证工具：`mpq.list` / `mpq.extract` / `mpq.verify` /
> `mpq.recover`（DIABDAT 静态字典已恢复 81 个真实文件名）。详见
> `docs/ARCHITECTURE.md`。阶段 1+ 的解码器（`src/decoders/`）待实现。

### 阶段 1：精灵与调色板解码（基本完成，2026-11-07）
- [x] 解码 `.pal` 调色板：`src/decoders/palette.cpp`（`loadPalette` 256×3→RGB、`paletteToRGBA`）。
- [x] 解码 `.cel` / `.cl2`（含帧组、RLE、透明）：`cl2.cpp`/`cel.cpp` 将 CEL/CL2 移植为 CLX
     （`cl2ToClx`/`celToClx`，忠实移植 DevilutionX `Cl2ToClx`/`CelToClx`），由 `clx.cpp` 的
      `decodeClx`/`decodeClxList` 处理帧组（sheet）、CL2 帧 RLE 与透明（alpha=0），
      输出 `DecodedFrame`（RGBA8，4B/px，行主序）。
- [x] PCX 解码：`src/decoders/pcx.cpp`（`decodePcx`，含内嵌调色板）。
- [x] 产出：任意精灵 → PNG 的离屏渲染：`src/decoders/png.cpp`（`writePNG`/`encodePNG`，
      调用 `src/decoders/deflate.cpp` 的 **RFC 1951 fixed-Huffman + 贪心 LZ77 压缩器**，
      无外部 zlib 依赖）+ `mpq-info/render_sprite.cpp`（串联 CL2/CEL→CLX→RGBA→PNG，
      把 `out/extracted/` 中已抽取精灵渲染为 PNG）。已验证 `plrgfx\warrior\wlu\wluas.cl2`
      渲染为 96×96 战士立姿（80 帧 = 8 方向×10 均可解码无错位）。
  - **修复 PNG 压缩器（2026-11-09）**：早期实现用 `stored`（未压缩）DEFLATE 块，
      导致 `scene.build` 预览 PNG 约 2.2 MB、base64 后超过 Firefox `data:` URL ~2 MB 上限，
      前端显示「无法显示的图片」/ 空图。改为真正的 fixed-Huffman + 贪心 LZ77 压缩器
      （`src/decoders/deflate.cpp` 的 `compressZlib`，配套 `decompressZlib` 做单测回环），
      并修正两处关键错误：`kLenExtra` 表偏移（RFC 1951 §3.2.5 中 257–264 为 0 额外位）
      与 length/distance 额外位/距离码的位序。真实 `scene.build(tristram)` 预览降为
      **257 KB PNG / 343 KB base64（0.33 MB）**，可由标准 `zlib.decompress` 无损解出。
      单测 `testDeflateRoundTrip` 穷举 length 3–258 × distance 全 30 码边界共 15 万+ 组合。
  - **注意**：CL2/CEL 文件**不存储帧宽**，渲染时需按 DevilutionX 动画表传入 `width`
    （如 warrior Stand=96、Attack/Death=128，见各 `sprites.tsv` 的 `stand`/`attack`… 列；
    怪物/导弹/物品/物件/NPC 各有对应宽度）。**`render_sprite` 已支持把 `<width>` 传 `auto` 自动查表**：
    按精灵路径判定家族（`plrgfx`/`monsters`/`missiles`/`objects`/`towners`），分别从
    `sprites.tsv`(`stand`/`walk`/`attack`(含武器为 `bow`)/`swHit`/`lightning`/`fire`/`magic`/`death`/`block`)、
    `monstdat.tsv`(`width`)、`missile_sprites.tsv`(`width`)、`objdat.tsv`(`animWidth`)、
    `towners.tsv`(`animWidth`) 取宽；`items`(CEL) 无宽度表，仍需手填。`towners/animals/cow.cel`
    是 `towners.cpp` 硬编码特例，不在 TSV 中，自动查表会回退到手动宽度。
  - 调色板用任意 256 色 `.pal` 即可出形，选对游戏调色板则颜色正确。
  - 单测：`out/unit_tests.cpp` 含 CLX/CEL/CL2 round-trip、**CL2/CEL 畸形输入与错误 `width`
    拒绝**、PCX、palette、**PNG write round-trip**；构建与运行见仓库根 `README.md` §1
    （解码器单测）与 §5（精灵渲染）。
  - [x] 修复 CL2 解码器越界崩溃（2026-11-08）：`src/decoders/cl2.cpp` 对所有文件偏移做边界
    校验、帧循环改为 `src < frameEnd`、坏数据抛异常转干净 JSON-RPC error。根因
    `cl2.cpp:24` 无边界的头部探测 + `cl2.cpp:64` 的 `!=` 循环让残缺 CL2
    （`monsters\darkmage\dmagew.cl2`，96 字节 stub）越界读触发 SIGSEGV。回归测试
    `testCl2RejectsMalformed`（单元）与 `testSpriteDecodeRejectsTruncatedCl2`（端到端）。
  - [x] 修复 CEL 解码器错误 `width` 崩溃（2026-11-08）：`src/decoders/cel.cpp` 同样加偏移
    校验、`src < srcEnd`、`int remaining`（原 `unsigned` 会下溢空转），错误宽度
    （`cow.cel` 传 64/96/160）不再 SIGSEGV。回归测试 `testCelRejectsMalformed`。

### 阶段 2：角色查看器（目标 2）

> **交付形态（2026-11-07 确定）**：以**浏览器查看器**实现，通过 WebSocket 调用本地
> `mcp-server` 的 `sprite.*` 工具取 RGBA 帧绘制；前端不读 MPQ/文件。后端主干（WS 桥 +
> `palette.load`/`sprite.decode`）已具备，详见 `docs/ARCHITECTURE.md` §10。先补两件
> 后端小事，再写前端壳——避免一上来就搭动画/合成框架。
> 关键认知：D1 的武器/护甲已 **bake 进文件名前缀**（`{c}{a}{w}`），"装备图层叠加"=
> **枚举前缀组合**，不是 alpha 合成多张图，勿过度设计通用合成器。

- [x] **后端：`sprite.decode` 支持 `width:"auto"`**：复用 `render_sprite` 的路径→家族→
      宽度表查表（plrgfx→`sprites.tsv`、monsters→`monstdat.tsv`、missiles→
      `missile_sprites.tsv`、objects→`objdat.tsv`、towners→`towners.tsv`；items 无表仍手填）。
      查表逻辑抽成共享模块 `src/decoders/sprite_width.cpp`，已三套件测试 + 端到端验证。
- [x] **后端：新增 `sprite.list`**：读 `recovered.json` + DevilutionX 数据表，枚举真实存在的
      精灵名并按 家族/职业/动作 归类（前端据此建目录树）。`mpq.list` 仅给占位名，不足以浏览。
      已加入 `integration_tests.cpp` 断言分类正确。
- [x] **前端壳（web/，JS ES Module+Canvas，零构建）**：连 `ws://localhost:18765` →
      `sprite.list` 取目录 → 点击即 `sprite.decode` 取 RGBA → Canvas 绘制；先做"选精灵→看单帧"
      （Frame 滑块翻帧）。环境无 TypeScript 工具链，故用纯 JS ES Module（模块划分与计划一致，
      需静态服务器托管，不能用 file://）。详 `web/README.md`、`docs/ARCHITECTURE.md` §10.3。
- [x] **前端：独立「角色查看器」面板 + 8 方向动画播放（2026-11-07 落地）**：在 `web/` 新增
      **浏览模式切换**（目录浏览 / 角色查看器）。角色面板按 `职业 → 动作 → 盔甲/武器` 选择器
      拼 `plrgfx\{class}\{c}{a}{w}\{c}{a}{w}{type}.cl2` 解码（装备组合即文件名枚举，无合成）。
      玩家 CL2 为方向优先（8 方向 × N 帧），前端按 `帧数/8` 切分，提供方向滑块 + ▶播放/⏸ +
      fps 速度控制；索引由 `sprite.list(family:"plrgfx")` 一次性构建，仅暴露 MPQ 真实存在的组合。
      改动 `web/index.html` + `web/src/main.js` + `web/styles.css`，后端无改动。详见 `web/README.md`
      与 `docs/ARCHITECTURE.md` §10.4。
- [x] **（可选保真）`sprite.decode` 应用 TRN**：已实现（2026-11-07），原理取自 DevilutionX 逆向。
      `sprite.decode` 新增 `apply_trn`（布尔，默认 false）与 `trn_name`（可选显式 TRN）参数，
      开启后按默认 `plrgfx\trn<class>.trn`（来自 `sprites.tsv` 的 `trn` 字段，即 `trn`+职业，
      如 `trnwarrior`）或 `trn_name` 取 256 字节 TRN，重映射调色板后再解码；返回 `trn_applied` /
      `trn_name`。**实测关键结论**：原版 MPQ 内 `plrgfx\trnwarrior/trnrogue/trnsorcerer/trnmonk.trn`
      **均不存在**（plrgfx 下仅 `infra.trn`/`stone.trn` 真实存在），故职业默认路径是**忠实的
      no-op**（`trn_applied:false`，仍用调色板上色），与游戏内玩家着色一致；`infra.trn`（红外视觉
      红染）/`stone.trn`（石化灰染）可经 `trn_name` 显式施加作效果演示。前端 `web/` 新增「应用
      TRN」开关 + 预设下拉（职业默认/infra/stone），默认关闭保留原调色板模式；详见 §10.2.3。
- [x] 枚举各职业所有动画模式（stand/walk/attack/hit/death/idle…，对应 `as/st/aw/wl/at/ht/dt…`）：
      由 `sprite.list(plrgfx)` 真实存在的 type 代码驱动动作下拉，按 `sprites.tsv` 列映射友好名。
- [x] 8 方向切换（玩家固定 8 方向，按 `帧数/8` 切分；怪物方向数另查动画表）：前端通用分组，
      方向数可选 1/2/4/8，plrgfx 默认 8。
- [x] 装备组合浏览（前缀 `c+a+w` 枚举：武器/护甲变化 = 不同文件名，直接枚举即可）：角色面板的
      盔甲/武器下拉仅列出该职业当前动作真实存在的组合。
- [x] 角色施法动作层渲染（`lm`/`fm`/`qm` 是**角色举手/发光动作**，不是火球之类攻击效果；
      真正的攻击效果在 `missiles\` 家族，见阶段 3.5）：`web/` 新增通用
      **特效叠加层（alpha 合成）**面板——选好基础精灵后，可把任意特效精灵解码为独立图层，按
      `(dir, frame)` 对齐后 alpha 叠加到底图上（见下方「特效叠加层」小节）。该面板覆盖三种浏览模式：
      角色（技能 `lm`/`fm`/`qm` 一键加）、怪物（Special `s` 动画一键加）、以及通用「任意精灵」选取器。
- [x] 产出：按“职业 × 动作 × 装备 × 技能特效”组合的查看数据（浏览器内实时查看；非合成位图）。

### 阶段 3：怪物 / NPC 查看器（目标 3）
- [x] 枚举怪物/NPC 精灵与动画模式：`sprite.list(family:"monsters"/"towners")` 由 DevilutionX 数据表
      （`monstdat.tsv` / `towners.tsv`）**合成**目录——因怪物/NPC 无 MPQ listfile，无法靠哈希恢复，
      故从数据表推导精灵名。怪物按动画字母 `n/w/a/h/d/s`（Stand/Walk/Attack/Hit/Death/Special，
      来自 `frames[6]` 与 `Animletter="nwahds"`）枚举，每条带 `frames_per_dir` 与自动查表宽度；
      NPC 为单方向 `.cel`（含硬编码的 `towners/animals/cow.cel`）。见 `web/` 新增「怪物 / NPC 查看器」
      模式：选 MPQ → 选怪物 → 选动作即解码并播放，方向数取自解码返回的 `directions`。
- [x] 怪物技能动作与特效叠加：动画模式已枚举并按 8 方向播放；特效叠加层（如 Special `s` 动画、`lm`/`fm`
      类）现已通过通用「特效叠加层（alpha 合成）」面板做 alpha 合成到怪物/NPC 帧上（与角色查看器共用同一
      叠加子系统，覆盖用户要求的四类作用域：怪物查看器里的怪物、角色查看器里的技能、通用底图+叠加选取器、
      monster 的 Special 动画）。
- [x] 产出：怪物/NPC 查看数据（浏览器内实时查看；后端 `directions` 字段由 `monstdat.tsv` 的
      `frames[6]` 切分方向，如僵尸 Stand=11 帧/方向 × 8 方向 = 88 帧 → `directions=8`）。
- [x] 修复 Hellfire 怪物批量解码失败（`extract failed (name not found?)`）：根因是前端只把
      「选中的单个 MPQ」传给 `sprite.decode`，而 Hellfire 怪物 CL2 实际在 `hellfire.mpq`。现改为把
      全部已知 MPQ 作为 `archives` 搜索栈，并把路径统一规范为 `/`（避免 `recovered.json` 里
      `/` 与 `\` 混用使同一 MPQ 被打开两次报 `cannot open archive`）。回归测试
      `testSpriteDecodeCrossArchive` 断言「单 MPQ 失败 / 全栈成功」；全量 325 条合成目录中
      116 条由此修好（用户报告的 6 条全部由失败转 OK）。
- [x] 修复目录幽灵条目与 cow 宽度（2026-11-08）：
      - `synthesizeMonsters` 排除 `worm\worm`（4 个 `availability=Never` 的 beta 占位怪物共用，
        MPQ 无对应 CL2，共 5 条）。**不按 `availability` 过滤**——其余 15 个 Never suffix
        （golem / unrav / diablo …）都真实存在，误删会丢真实精灵。
      - `synthesizeTowners` 从 `TOWN_COW` 行读取 `animWidth=128` 填入硬编码的
        `towners\animals\cow.cel`；`autoWidth` 增加 `TOWN_COW` 回退（cow 的 `animPath` 为空、
        CEL 路径只在 `towners.cpp` 硬编码），使 `width:"auto"` 能解码（96 帧）。
      - 回归测试 `testSpriteListCatalogFixes`（目录过滤 + 宽度）与 `testSpriteDecodeCow`（auto
        解码 + 错误宽度不崩）。全量 320 条回归 ok=319，仅剩 dmagew 残缺 CL2 的预期干净错误。

### 特效叠加层（alpha 合成）
- 通用叠加子系统：在任一浏览模式下选中基础精灵后，右侧「特效叠加层」面板（默认折叠，点「展开」）可把
  任意特效精灵作为独立图层解码，并按 `(dir, frame)` 对齐后 alpha 合成到底图当前帧上。
  - 快速加：角色模式列出当前职业的 `plrgfx\{c}\{caw}\{caw}{lm/fm/qm}.cl2`（闪电/火/魔法）；怪物模式列出
    该怪物的 Special `s` 动画（`..._{nwahd}.cl2` → `..._s.cl2`）。
  - 通用加：下拉列出当前目录所有精灵，可任意选取做底图叠加（如把一件物品/技能特效叠到角色上）。
  - 每层可单独启用/停用、设 `x/y` 偏移、移除。元信息行显示「· 叠加层 N」。
- 实现：`web/src/viewer.js` 的 `drawComposite(画布, 底图RGBA, w, h, 叠加数组)`；叠加层状态在
  `web/src/main.js`（`overlayLayers`、`lastDecodeArgs`、构建参数复用 `buildDecodeArgs` 以共享调色板/TRN）。
  切换基础精灵时清空叠加层并刷新快速加按钮。

### 阶段 3.5：技能 / 战斗查看器（全阶段视觉时间线）
- [x] 需求澄清：`plrgfx` 的 `lm/fm/qm` 是**角色施法动作**；火球 / 火墙等**攻击效果**在
      `missiles\` 编号文件。两者都需要，且一个技能的施放要显示**所有阶段**
      （施法 → 投射 → 命中 → debuff）；技能加在目标身上的 debuff 视觉也要。
- [x] 玩家技能列表：`spelldat.tsv` 36 个基础技能 + Hellfire 15 个 = 51 个，`missiles` 列给出 MissileID。
- [x] 怪物远程攻击列表：`monstdat.ai` + `Source/monster.cpp::GetMissileType(ai)` 真实 switch 转录，
      共 14 类 AI 攻击（含 default→Arrow）。
- [x] 怪物远程攻击补齐“AI 内直接 AddMissile”的缺失入口（`spell_tables.h::monsterAttackForms()`）：
      Counselor 按智力 0..3 分别发 Firebolt/ChargedBolt/LightningControl/Fireball，近身另发
      FlashBottom+FlashTop；Familiar 撤退时发 Lightning；Gloom 远距离 Rhino 冲撞；Mega 近身
      InfernoControl。每个形态单独成条（id 为 `AI` 或 `AI.variant`），带 `variant`/`condition`，
      `monsters` 按智力或怪物 id 收窄，怪物攻击条数 14 → 22。
- [x] 合并 `unique_monstdat.tsv`：唯一怪的 `ai`/`intelligence` 覆盖基础怪，解决归属错误——
      Hork Demon（唯一怪 `ai=HorkDemon`）的 `HorkSpawn` 可达；Zhar/Lazarus（`ZharAi`/`LazarusAi`
      委托 `CounselorAi`，用 `normalizedMonsterAi()` 归一）按其智力 3 归入 `Counselor.Fireball`；
      Red Vex / Black Jade（`LazarusSuccubus`）与 `AcidUnique` 也已进入各自条目；怪物攻击 22 → 25。
- [x] 怪物特殊能力入口 `abilities[]`（`spell_tables.h::monsterAbilities()`，转录自引擎实现）：
      `Blink` 受重击瞬移（`Teleport()`）、`AcidPuddle` 酸液怪死亡留酸池、`Hidden` 潜行系隐身
      （`SneakAi` 的 `MFLAG_HIDDEN`）；每条带 `monsters`/`trigger`/`source_ref`，`ability_count=3`。
      核实结论：Diablo 1 / Hellfire **没有** Diablo 2 式词缀——`Thunderstorm`/`Waller`/`Vortex`/
      `Shielding` 在 `Source/` 中完全不存在，故不伪造条目，相关说明写入 `docs/SPELLS.md`。
- [x] 陷阱法术入口 `traps[]`（`spell_tables.h::trapSpecs()`，转录自 `Source/objects.cpp`）：
      `OBJ_TRAPL/R` 墙陷阱按 `GenerateRnd((effLevel/3)+1)` → Arrow / Firebolt(有效层≥3) /
      LightningControl(≥6)；`OBJ_TCHEST1..3` 带陷阱宝箱 Arrow / FireArrow / Nova，地狱火扩到
      RingOfFire / StealPotions / StealMana；地狱火 `OBJ_FLAMEHOLE`/`OBJ_FLAMELVR` 为对象火焰
      动画 + 直接伤害（记 `FireWallControl`，无 missile 图形）。每条带 `objects`/`trigger`/
      `source_ref`，`trap_count=10`，`spell.list` 总计 89 条。
- [x] 后端 `spell.list`：四表 join（`spelldat` / `misdat` / `missile_sprites` / `monstdat`），
      输出 `{count,spell_count,monster_attack_count,cast_action_map,impact_catalog[],spells[],monster_attacks[]}`；
      每条目四阶段 `stages[]`，`sprite` 层带编号 `missiles\{name}{N}.cl2` 清单 + 显式 `width`，
      `trn` 层带目标 debuff 重映射；命中阶段按命名约定自动关联 + override + `impact_catalog` 候选。
      纯 join 逻辑在 `src/tools/spell_tables.h`（header-only，不依赖 json，便于单测）。
      每条目另带 `name_zh`（中文名，解析 `Translations/zh_TW.po`→`zh_CN.po` 的 `msgctxt "spell"；
      `.po` 中零宽/格式字符已清理，含 `{:` 占位符的坏翻译自动跳过回退英文名）与 `description`
      （按解析出的阶段链 + 玩家法力/智力消耗合成的中文说明）；并抽取**法术级别参数**：`level`
      （`spelldat.bookLevel` 法术阶）、`staff_level`、`mana_cost`、`mana_multiplier`、`min_mana`、
      `staff_min/staff_max`，以及 `mana_by_level`（按 `GetManaAmount` 公式算出的 1..bookLevel
      各级基础法力 `{level, mana}`，含 `min_mana` 下限、未乘职业系数）。另含 `visual_by_level`
      （玩家 1..bookLevel / 怪物 1..16 各级**视觉参数**：持续时间 `duration_ticks`/`duration_sec`、
      弹速 `speed`、范围 `range_tiles`/`radius_tiles`、弹射物数 `count` 及法术特有字段如
      `segments`/`wall_length_tiles`；各法术硬编码公式不同，由 `src/tools/spell_visual_model.h`
      逐法术从 DevilutionX handler 转录，含角色/地下城层依赖项标 `_note`，无级别缩放为空）。
      已**全量覆盖**全部 23 个可升级玩家法术（含 Fire Wall/Chain Lightning/Flame Wave/Charged Bolt/
      Holy Bolt/Guardian/Stone Curse/Inferno 等级别缩放项，及 Firebolt/Fireball/Elemental/Blood Star/
      Bone Spirit/Town Portal/Phasing/Teleport/Flash 等常量视觉项；Healing/Golem/ManaShield/Telekinesis/
      HealOther 为瞬发无弹道，visual_by_level 为空）与全部 25 条怪物攻击（16 类 `GetMissileType` AI + 9 个 AI 内直接发弹形态）
      （Arrow/BloodStar/Acid/Firebolt/各 Flare/MagmaBall/ThinLightningControl/DiabloApocalypse 等）。
      前端信息区同步显示法术等级、各级法力曲线与**各级视觉参数表**。
- [x] 前端第 4 种浏览模式「技能 / 战斗查看器」：`web/src/viewer.js` 新增 `drawScene`
      （多 actor 绝对定位，抽 `blitRgba` 复用，`drawComposite` 行为不变）；同屏两角色 + 效果；
      自动时间线连播四阶段 + 阶段 stepper；debuff 用 `apply_trn` 对目标层近似着色。
      角色前缀从 `charIndex.combos` 取真实存在的组合（不硬编码 `l+n`）。
- [x] 以施放者为中心的方向旋转（2026-11-08）：控制条「方向」滑块在战斗模式下生效（0–7），
      `combatLayout()` 固定施放者于场景中心、按方向向量重算目标位置，施放者朝向 = d、
      目标朝向 = (d+4)%8（回身面向施放者）；`combatCasterAnchor()` 从 `cast_action.anchor`
      判定施放者，故「怪物远程攻击」时施放者/目标互换、布局朝向随之变化。
- [x] 阶段内不切换方向（2026-11-08）：解码结果按方向拆为 `FrameSet.groups[]`
      （`makeFrameSet` / `frameSetAt` / `combatGroupIndex` / `frameSetBudget`）；
      时间线帧预算取各方向帧数最大值（方向无关），播放只推进当前方向组内的帧号。
      核实 `missile_sprites.tsv` 的 `numFrames>1` 编号文件是**方向**
      （`misdat.cpp::LoadGFX` + `spritesForDirection(Direction16)`），不再平铺成跨方向动画；
      cast 阶段也从整条 160 帧改为单方向 20 帧。投射物沿 施放者→目标 插值飞行动画。
- [x] 回归：`testSpellTables`（单元）、`testSpellListSynthesis`（集成）、stdio 端到端
      （51 技能 / 25 怪物攻击 / 3 特殊能力 / 10 陷阱 / 17 命中候选，Fireball 的 17 个特效文件全部可解码）、
      前端 42 个解码契约预演全通过；全量 320 条目录回归不退化；
      方向不变量 79 项断言 + 41 个前端解码契约 91 项断言全 PASS。


#### 技能与法术资源总览（对照 `docs/SPELLS.md`）

> `docs/SPELLS.md` 是法术内容的参考整理（其中混有来自暗黑 2 的说法）；本节把它的分类与
> 项目实际实现对齐，作为阶段 3.5 的验收清单。所有条目均由 `spell.list` 输出，数据来源为
> `spelldat` / `misdat` / `missile_sprites` / `monstdat` / `unique_monstdat`，陷阱另用 `objdat` 的 `OBJ_*` 宿主。

| 分类 | 内容 | `spell.list` 条目 |
|---|---|---|
| 玩家可学法术（法术书 4 页） | 火矢、充能弹、圣光弹、治疗、治疗他人、地狱火、复活、火墙、隔空取物、闪电、传送门、闪光、石化诅咒、相位术、法力护盾、元素召唤、火球、火焰波、连锁闪电、守护者、新星、魔像、传送、天启、骨灵、鲜血之星、虚化 | `spells[]`（本体 27） |
| 地狱火新增（第 5 页） | 闪电墙、献祭、扭曲、反射、狂暴、火焰之环、搜寻、小丑、法师、法力 | `spells[]`（10） |
| 角色固有技能 | 修理（战士）、拆陷（浪人）、法杖充能（法师）、搜寻（僧侣）、鉴定（诗人）、狂暴（野蛮人） | `spells[]`（与上重叠，合计 51） |
| 怪物远程攻击 | `GetMissileType()` 16 类 AI + AI 内直接 `AddMissile` 9 形态（Counselor 四弹 + Flash、Familiar 闪电、Gloom 冲撞、Mega 地狱火、Hork Demon 召唤） | `monster_attacks[]`（25） |
| 怪物特殊能力 | Blink 受击瞬移、酸液怪死亡留池、潜行系隐身 | `abilities[]`（3） |
| 陷阱法术 | 墙陷阱（箭矢 / 火焰弹 / 闪电）、带陷阱宝箱（箭矢 / 火焰箭 / 新星 / 火焰之环 / 偷药水 / 吸法力）、地狱火喷火口与火焰拉杆 | `traps[]`（10） |

**与 SPELLS.md 的差异（已逐条核实并更正）**：

- Teleporter / Thunderstorm / Waller / Vortex / Shielding 属**暗黑 2** 怪物词缀；DevilutionX 中只有
  `MT_BLINK` 的受击瞬移（对应 Teleporter），其余四项在 `Source/` 里无任何实现，故不输出假条目。
- 宝箱陷阱并不存在“分解（Disintegrate）”，实际第 5、6 种是 `StealPotions`（偷药水）与
  `StealMana`（吸法力）。
- Finger Mage 吸法 / 眩晕：`monstdat.tsv` 无此怪物，不适用。
- 玩家侧 51 条中 `DoomSerpents` / `BloodRitual` / `Invisibility` / `Rage` 是引擎未使用 ID
  （`bookLevel=-1`），`RuneOfFire/Light/Nova/Immolation/Stone` 是陷阱符文，均非可学法术，
  但保留在目录里以便与数据表完整对照。

**字段口径**：`name_zh`（`Translations` 中文名）+ `description`（中文合成说明）；玩家法术另有
`level` / `staff_level` / `mana_cost` / `mana_multiplier` / `min_mana` / `mana_by_level` /
`visual_by_level`；多形态怪物攻击有 `variant` / `condition`；特殊能力与陷阱有 `trigger` /
`source_ref`（DevilutionX 源码位置），陷阱还有 `objects`（宿主 `OBJ_*`）；所有条目共用
`missiles` 与四阶段 `stages[]`（cast → projectile → impact → debuff）视觉链。


**字段示例**（`spell.list` 返回的 `spells[]` 单条 = Fireball，节选：`…` 表示省略的重复项）：

```json
{
  "id": "Fireball",
  "name": "Fireball",
  "name_zh": "火球术",
  "description": "火系法术。法力消耗 16，最低智力 48。法术等级 8（每升 1 级法力 -1，最低 10）。投射物：Fireball；命中：MagmaBallExplosion；状态：无。",
  "source": "player",
  "element": "Fire",
  "cast_action": "fm",
  "level": "8",
  "staff_level": "7",
  "mana_cost": "16",
  "mana_multiplier": "1",
  "min_mana": "10",
  "staff_min": "40",
  "staff_max": "80",
  "mana_by_level": [
    { "level": 1, "mana": 16 },
    { "level": 2, "mana": 15 },
    …（共 8 级；L7/L8 触底为 min_mana = 10）
  ],
  "visual_by_level": [
    { "level": 1, "params": { "duration_ticks": "256", "duration_sec": "4.27", "speed": "16" } },
    …（各级参数相同，共 8 级）
  ],
  "missiles": ["Fireball"],
  "stages": [
    { "id": "cast", "label": "施法", "layers": [
      { "kind": "cast_action", "role": "caster", "anchor": "player", "action": "fm", "element": "Fire", "confidence": "auto" } ] },
    { "id": "projectile", "label": "投射", "layers": [
      { "kind": "sprite", "role": "projectile", "anchor": "world", "graphic_id": "Fireball",
        "files": ["missiles\\fireba1.cl2", "…（共 16 个编号文件，即 16 个方向）"],
        "width": 96, "frames": 14, "num_files": 16, "element": "Fire", "confidence": "auto" } ] },
    { "id": "impact", "label": "命中", "layers": [
      { "kind": "sprite", "role": "impact", "anchor": "monster", "graphic_id": "MagmaBallExplosion",
        "files": ["missiles\\magblos.cl2"], "width": 128, "frames": 10, "num_files": 1,
        "element": "Fire", "confidence": "override" } ] },
    { "id": "debuff", "label": "状态", "layers": [] }
  ]
}
```

其余数组的差异字段：`monster_attacks[]` 多 `monster_ai` / `monsters` / `default_ai` / `variant` / `condition`；
`abilities[]` 多 `ability` / `monsters` / `trigger` / `source_ref`；`traps[]` 多 `trap` / `objects` / `trigger` / `source_ref`。

### 阶段 4：地图还原（目标 1）

> **状态：完成（2026-11-09 收工）。**
> 解码 + 离屏渲染管线、`map.build`、完整场景复原 `scene.*`（Tristram + L1–L4）、静态光照
> （dLight）、A* 寻路、`level.inspect` 关卡 tile 查看器、前端场景/关卡查看器与三套测试
> 全部落地（2026-11-08 初版，2026-11-09 完成黑块/索引/光照/寻路/tile 纵向翻转修正）。
> 忠實移植 DevilutionX `Source/levels/` 的 DUN→TIL→MIN→level CEL 拼装与
> `scrollrt.cpp`/`dun_render.cpp` 的两遍等距合成，后端纯 C/C++（`src/decoders/level_*.cpp` +
> `src/tools/map_tools.cpp` + `src/tools/scene_tools.cpp` + `src/tools/level_tools.cpp`），无 Node 运行时。

> **2026-11-09 重要修正（黑块根因 + 索引约定）**：初版渲染把背景留成全透明
> （RGBA alpha=0，任何查看器压到黑就成了「黑块」），对 `mi==0` 的格子直接跳过，
> 且把 TIL 微瓦片索引误当成 1-based。经 DevilutionX 源码 + 真实 MPQ 数据核对，
> 三点全部更正，详见下方「游戏场景如何组织」：
> ① **背景填充不透明色**（`MapRenderOptions.background`），输出 0 透明像素；
> ② **TIL 微索引是 0-based**（`micros[mi]`，`0` 是合法地板，空用 `kNoMicro` 哨兵）；
> ③ **场景没有空格**：DevilutionX 把整个 `dPiece` 预填成地板微瓦片，本实现用
> `prefillMicro` 复现，并改为 **两遍绘制**（`DrawFloor` 地板菱形 + `DrawCell` 墙体）。
> 修正后 `scene.build(tristram)` 返回 `transparent_pixel_count == 0`，地图为完整无空洞的等距菱形。

- [x] **TIL 解码**：`src/decoders/level_decode.cpp::loadTil` —— 每 8 字节 = 4 个
      little-endian uint16（micro1..micro4，即 TL/TR/BL/BR 四个角微瓦片索引）。
- [x] **MIN 解码**：`loadMin(buf, blocks, microLen)` —— 每 piece 按 DevilutionX
      `SetDungeonMicros` 的**块源偏移置换**（`(blocks-2)+(b&1)-(b&0xE)`）读取至多 16 个
      `LevelCelBlock`（data 低 12 位 = 1-based 帧号、高 3 位 = TileType）；`blocks`=10（原版）
      /16（地狱火），`microLen`=10/12。
- [x] **DUN 解码**：`loadDun` —— 头 2 个 uint16（宽/高）+ 宽×高 个 uint16 兆瓦片层
      （0=空），并可选地读取微尺度透明层（位于 tile 层后 3 个 (2w×2h) 层之后）。
- [x] **SOL 解码**：`loadSol` —— 按**微瓦片 piece id** 索引的碰撞/光照标志位
      （`SOLData[dPiece[x][y]]`，`dun_tile_data.hpp:219-221`）；磁盘文件只有 `pieceCount`
      字节（`l1.sol=453`、`town.sol=1258`），读入 `MAXTILES=1379` 的零填充表，
      其余项保持 0；标志位 Solid/BlockLight/BlockMissile/Transparent/…（与 `dun_tile.hpp` 一致）。
- [x] **兆瓦片→微瓦片网格拼装**：`buildMicroGrid`（忠实 `DRLG_LPass3`）——DUN 每个兆瓦片
      展开为 2×2 个微瓦片索引，网格为 (2w)×(2h)。**关键约定（2026-11-09 更正）**：TIL 的
      微瓦片索引是 **0-based 原样值**（`micros[mi]`，见 `gendung.cpp:607-610`、
      `town.cpp:52-55`、`scrollrt.cpp:551-553`），`0` 是合法地板；真正界外用 `kNoMicro`
      哨兵表示。`LevelCelBlock.frame()` 的 1-based 是 **CEL 帧号**的另一套约定，二者不可混淆。
      实证：`l1.til` 微索引 0..452 ↔ `l1.min` 453 pieces；`town.til` 0..1257 ↔ `town.min` 1258 pieces。
- [x] **level CEL 重编码**：`reencodeLevelCel` —— 忠实 `ReencodeDungeonCels`，把原始 level CEL
      逐帧重编码为定长缓冲（Square=1024B / 三角=512B / 梯形=784B / 透明方保留 RLE），便于渲染器
      直接读取；`buildFrameTypes` 用每个微瓦片引用的 CEL 块推导每帧 TileType。
- [x] **帧解码 → RGBA**：`decodeReencodedFrame` + 各 TileType 分支
      （`DecodeLeft/RightTriangle/Trapezoid/Square/TransparentSquare`）——按 TileType 还原 32×32
      调色板索引像素；新增 `decodeReencodedFrameAs` 支持**强制按三角形解码**（`DrawFloorTile`
      无条件把 mt[0]/mt[1] 当左/右三角形）；**已修调色板索引为 4 字节 RGBA 步长**
      （`paletteToRGBA` 产出 1024 字节表），修正了此前 3 字节步长导致的彩虹噪点。
- [x] **等距合成渲染（2026-11-09 重写为忠实两遍绘制）**：`renderMap` 移植 `DrawGame` →
      `DrawFloor` + `DrawTileContent/DrawCell` + DevilutionX 屏幕轴：
      `screenX=(i-j)*32`、`screenY=(i+j)*16`（2026-11-09 修正：此前用 `(j-i)`/`-(i+j)`，
      相对游戏原图整体旋转 180°）。**Pass 1** 为每个格子把 `mt[0]/mt[1]` 强制画成
      左/右三角形、组成 64×32 地板菱形（完整地面平面，无空洞）；**Pass 2** 从远到近
      （升序 `i+j`）把 `mt[2..microLen-1]` 成对向上叠墙（每对高 32px），`hasValue()` 守卫。
      每像素先填 `opts.background`（默认不透明黑）→ **输出 0 透明像素，黑块消失**；
      `kNoMicro` 格子用 `prefillMicro` 补地板；SOL 按微瓦片 piece id 取标志（IsFloor 判定）。
      `sceneProjection(gw,gh,microLen)` 抽出包围盒，供渲染器与叠加层共用。
- [x] **`map.build` MCP 工具**：`src/tools/map_tools.cpp::toolMapBuild` —— 串起上述全链路，
      写完整地图 PNG + ≤400px 预览 PNG + 每格标志 sidecar JSON 到 `out/maps/`，并返回摘要与
      base64 预览；已注册到 Tool Registry（stdio + WS 双传输复用）。2026-11-09 同步改用修正后的
      两遍渲染 + 不透明背景 + 空格预填。
- [x] **单元/集成测试**：
      - `out/unit_tests.cpp`：TIL/MIN/DUN/SOL 解析、`buildMicroGrid` 0-based/kNoMicro 拼装、
        Square/Triangle CEL 重编码与解码、强制三角形解码、`encodePNG`、
        `renderMap` 两遍绘制 + 不透明背景 + 0 透明像素 + 空格预填 + frame()==0 守卫、
        **`level_light`（LightTables Normal/Hell/Soft、falloff/cone、`buildLightMap`）与
        `renderMap` 逐格光照着色**、**`level_path` 8 方向 A\*（开放/墙体/斜角/Solid 目标/maxSteps）**、
        `buildTownScene`（4 sector + 预填 426/218）、`buildDungeonScene`（Dirt 预填 + DUN@16,16）、
        `sceneProjection`/`sceneGridDims`。
      - `out/integration_tests.cpp`：`testLevelMap` 从 `DIABDAT.MPQ` 抽真实 L1 资源跑通全链路并
        断言 **0 透明像素**；`testSceneTools` 用 `scene.list`/`scene.build`/`scene.path` 复原
        Tristram 城镇，断言 96×96 网格、0 透明像素、base64 PNG 合法、叠加层/每格标志/
        `walkable` 数组、`scene.path` 起止格与原生/预览坐标；`l1_rnd2` + `lighting:true` 断言
        环境光 15 / 中心火把 / `meta.light` 尺寸；`l4_diab1` 断言 44×44、microLen 12、0 透明像素。
  - 构建/运行见仓库根 `README.md` §测试 与 §6（关卡/场景）。
- [x] **tiles / objects 绘制修正（2026-11-09）**：
      - **Pass 1 只画 `IsFloor` 格子**（`!Solid && !BlockMissile`）；此前对墙体/实心格也强行画地板菱形，
        会把墙基/树叶数据按三角形解码，产生黑块/彩色噪点。
      - **`TransparentSquare` 地板按真实 RLE 解码**；此前强制三角形解码会把 RLE 控制字节当调色板索引，
        正是截图中橙色噪点的来源（DevilutionX `ReencodeDungeonCels` 会把它拆成 triangle floor + foliage）。
      - **`dSpecial` 城镇特殊覆盖层**：`townSpecialFrameId()` 转录 `InitTownPieces`（micro 359/357/128/…/215），
        `scene.build` 从 `levels/towndata/towns.cel`（64px 宽、224px 高）解码并底部锚定绘制树木/拱门；
        `meta.special` / `special_count` 暴露每格 id。
      - **Tristram `FillTile` 补丁**：应用非任务态的水井/喷泉 `(60,70,71)` 与传送入口 `(48,20,320)`、
        `(16,68,332)`、`(16,70,331)`，这些是 `dPiece` 内容而非 automap 层。
      - **tile 纵向翻转修复（水池不圆的根因）**：`DecodeLeft/RightTriangle`、`DecodeLeft/RightTrapezoid`
        输出 **top-down** RGBA，而 `DecodeSquare/TransparentSquare` 输出 **bottom-up**（CEL 源行 0 为底），
        `blitFrame` 又统一做了一次翻转，导致三角/梯形 tile 上下颠倒。水池的 `RightTrapezoid` 环因此错位；
        现在所有 level CEL 解码器统一输出 top-down，`blitFrame` 不再翻转，水池恢复封闭圆形。
      - **`level.inspect` + `web/level.html` 查看器**：按 megatile/micro/frame 粒度查看
        `town.til`→`town.min`→`town.cel` 的 block 类型、帧号、原始 data、每帧 PNG 与等距组装预览；
        默认预填 Tristram `megatile=71`（水池），可直接核对 pieces 257–260 / frames 594–605。
      - **未处理**：地牢 DUN 的 object layer（箱子/火把等 `objects/*.cel`）与 `dTransVal`/`TransList`
        的透明遮挡，仍属后续项。
- [x] **静态光照（dLight，2026-11-09）**：`src/decoders/level_light.cpp` 忠实移植
      `Source/engine/light_tables.cpp::MakeLightTable` 与 `Source/lighting.cpp::DoLighting`：
      生成 16 级 `LightTables`（Normal / Hell / Soft）、`LightFalloffs[16][128]`、
      `LightConeInterpolations[8][8][16][16]`，按场景环境光（城镇 0 / 地牢 15）+ 点光源计算
      每格 `dLight`；`renderMap` 每格用对应的 lit palette 解码瓦片（不是近似 RGB 乘）。
      `scene.build` 新增 `lighting` / `ambient` / `lights[]` / `auto_light` 参数，`meta.light`
      输出每格 0..15 光照级别；默认 `lighting:false` 保持 FullyLit 既有行为。Hell 关（L4）
      使用 `LightStyle::Hell` 血液墙色阶，Nest/Crypt 可走 Soft 曲线。前端场景查看器新增
      「光照（dLight 暗部 + 中心火把）」开关。
- [x] **寻路（A\*，2026-11-09）**：`src/decoders/level_path.cpp` 在场景 SOL `Solid` 网格上做
      8 方向 A*，步长代价照搬 `Source/engine/path.cpp`（直行 100 / 斜行 101），
      对角只要求目标格可行走（与 `CanStep` 对斜向不设限一致），目标格可 Solid（同 `FindPath`
      允许 targeting 非可行走目标）。新增 `scene.path` MCP 工具：输入 `from`/`to` 微瓦片坐标，
      返回 `found`/`cost`/`steps`/`walkable_count` 与每格原生/预览屏幕坐标；`scene.build` 的
      sidecar 同时输出 `walkable` 数组。
- [x] **更多关卡预设（2026-11-09）**：`sceneCatalog` 由 Tristram + 2 个 L1 扩到
      **6 个完整场景**：`tristram`、`l1_rnd2`、`l1_skngdo`、`l2_blind`（L2，prefill 12）、
      `l3_anvil`（L3，prefill 8）、`l4_diab1`（L4，`microLen=12`/`blocks=16`、prefill 30、
      Hell 光照）；palette 分别为 `l2_2.pal` / `l3pfoul.pal` / `l4_1.pal`（均已实证存在）。
- [ ] **逐像素光照 / lightmap（可选，后续）**：当前为 DevilutionX 默认的**每格平地光照**
      （`LightTables` + `dLight`），未实现 `perPixelLighting` 的 marching-squares 插值光图；
      地牢 DUN object layer 与 `dTransVal`/`TransList` 透明遮挡尚未接入。这些属于运行时/细节
      保真项，已从阶段 4 主目标中剥离，不影响「完整地图 + 光照/寻路信息」的交付。

**阶段 4 验收（2026-11-09 收工）**：

| 验收项 | 状态 |
|---|---|
| TIL / MIN / DUN / SOL / level CEL 解码与 0-based 索引 | ✅ |
| 两遍等距绘制（地板菱形 + 墙体）、不透明背景、空格预填、0 透明像素 | ✅ |
| 完整场景：Tristram + L1/L2/L3/L4（6 个 preset） | ✅ |
| 场景输出原生 PNG + ≤1000px 预览 + SOL 叠加层 + sidecar JSON | ✅ |
| 静态 dLight（Normal/Hell/Soft、点光源、前端开关） | ✅ |
| SOL 碰撞网格 A* 寻路（`scene.path`） | ✅ |
| `level.inspect` 关卡 tile 查看器（megatile/micro/block/frame/组装） | ✅ |
| `dSpecial` 城镇树木/拱门覆盖层 + Tristram FillTile（水池/传送入口） | ✅ |
| tile 纵向翻转修复（triangle/trapezoid 与 square 统一 top-down） | ✅ |
| 前端场景查看器 + 关卡 tile 查看器（`web/`） | ✅ |
| 单元 / 集成 / core 三套测试 + mcp-server 构建 | ✅ |

### 游戏场景如何组织（对照 DevilutionX，2026-11-09 澄清）

> 用户提出「测试输出仍有黑块、看起来不完整」，追根是**把一个孤立 `.dun` 预设当成完整场景**，
> 且渲染器不符合引擎的场景模型。以下为经 `D:\work\xt\diablo\DevilutionX` 源码 + 真实 MPQ
> 数据核对后的场景组织方式，是 `scene.*` 工具与前端场景查看器的设计依据。

**三层数据模型**（`Source/levels/gendung_defs.hpp`、`dun_tile_data.hpp`、`dun_tile.hpp`）：

| 层 | 结构 | 内容 |
|----|------|------|
| ① 兆瓦片层 | `dungeon[40][40]`（DMAXX/DMAXY=40） | 1-based 兆瓦片 id（0=空）；城镇另由 `automap.dun` 填充此层 |
| ② 微瓦片层 | `dPiece[112][112]`（MAXDUNX/Y=16+40*2+16） | 每格一个 **0-based 微瓦片 piece id**（原样来自 TIL），四周 16 格为预填边界 |
| ③ 瓦片图形 | `.til` → `pMegaTiles[]`（4 个微索引）、`.min` → `DPieceMicros[1379]`（每 piece ≤16 个 `LevelCelBlock`）、`.cel` → 32×32 像素帧 | 渲染基元 |

- `LevelCelBlock`（16-bit）：`data` 低 12 位 = **1-based CEL 帧号**、高 3 位 = `TileType`
  （Square/TransparentSquare/LeftTriangle/RightTriangle/LeftTrapezoid/RightTrapezoid）；
  `data==0` 表示「无帧」。`.min` 的块按 DevilutionX `SetDungeonMicros` 的置换
  `pieces[blocks-2+(b&1)-(b&0xE)]` 读入。
- **索引约定（关键，初版曾弄错）**：`dungeon[i][j]` 与 DUN word 是 **1-based** 进 `pMegaTiles`
  （`-1`）；但 **TIL 的 micro1..4 是 0-based 微瓦片索引，原样写进 `dPiece`**
  （`DPieceMicros[dPiece[x][y]]`，无 `-1`）。`SOLData` 同样按微瓦片 piece id 索引。

**场景组装**：

- **Tristram 城镇（初始场景 `DTYPE_TOWN`，`currlevel=0`）**：`DrlgTPass3()`（`town.cpp:193-256`）
  先把整个 `dPiece` 预填微瓦片 **426**，再 `FillSector()` 铺 4 个 sector `.dun`
  （`town_data.cpp:116-125`：sector1s@(46,46) 25×25、sector2s@(46,0) 25×23、
  sector3s@(0,46) 23×25、sector4s@(0,0) 23×23）。DUN word=0 → 微瓦片 **218**，
  word≥1 → `pMegaTiles[word-1]` 的 4 个微索引展开成 2×2。城镇 `microLen=16`/`blocks=16`，
  资源 `levels\towndata\town.{cel,til,min,sol,pal}`（均在 DIABDAT.MPQ 实证存在）。
- **地牢（L1 等）**：`DRLG_LPass3()`（`gendung.cpp:582-615`）用「Dirt」兆瓦片
  （L1 `Dirt=22` → `pMegaTiles[21]`）预填整个 `dPiece`，再把 DUN 层叠在偏移 (16,16)。
  L1 `microLen=10`/`blocks=10`；L4/Hell 为 12/16。

**渲染（两遍，`scrollrt.cpp:1231-1285` + `dun_render.cpp`）**：`DrawGame` 先 `DrawFloor`
（每格 `mt[0]` 强制左三角 + `mt[1]` 强制右三角 → 64×32 地板菱形，铺满地面），
再 `DrawTileContent`/`DrawCell`（`IsFloor` 判定：SOL 非 Solid/非 BlockMissile；墙体 `mt[2..]`
成对向上叠，每对高 `TILE_HEIGHT=32`，`hasValue()` 守卫）。**因此真实场景没有空格**——
初版把 `mi==0` 当空跳过、背景留透明，才是黑块的真正来源。

**本实现**（`src/decoders/level_scene.cpp` + `src/tools/scene_tools.cpp`）：
`buildTownScene`（4 sector + 预填 426/218，裁到 96×96 sector bbox 以约束图像）、
`buildDungeonScene`（Dirt 预填 + DUN@16,16）、`sceneCatalog`（`tristram` + L1/L2/L3/L4
共 6 个预设）、`loadSceneAssets`（工具侧共享抽取/组装）。`scene.build` 写原生 PNG 到
`out/scenes/`，整数降采样返回 `image_base64` + 对齐的 `overlay_base64`（网格线 + SOL
着色）+ 每格标志 `meta`，可选 `lighting` 走 `buildLightMap` + `LightTables`；前端第 5 种
浏览模式缩放/平移/叠加/光照。

**修复空图/无法显示（2026-11-09）**：两处根因。(1) PNG `data:` URL 过大且不可解：
`src/decoders/deflate.cpp` 早期用未压缩 stored DEFLATE 块，`scene.build(tristram)`
预览 PNG ≈2.2 MB、base64 后超 Firefox ~2 MB 上限 → 前端显示「无法显示的图片/空图」；
改为 fixed-Huffman + 贪心 LZ77 后降为 257 KB PNG / 343 KB base64（标准 zlib 可无损解），
见阶段 1 条目。(2) 夜景过暗：`town.pal` 本身正常（mean 亮度 73），但等距菱形大面积
近黑 + 暗色地砖使均值亮度仅 13、67% 像素 <24，观感近似空图。前端「提亮（夜景）」
开关改为**默认勾选**（`web/index.html`），`loadScene` 每次加载按开关状态施加
`.bright` 类（`web/src/main.js`），CSS 滤镜由 `brightness(1.7)` 增强为
`brightness(1.7) contrast(0.85) saturate(1.1)`（用轻度降对比抬暗部、菱形背景仍近黑），
可见像素（亮度≥40）从 8% 提升到约 38%。

**已补（2026-11-09）**：静态 `dLight` 光照（`level_light.cpp`，`scene.build(lighting:true)`）、
SOL 碰撞网格 A* 寻路（`scene.path`）、L2/L3/L4 预设场景。`scene.build` 默认仍 FullyLit，
可显式开启光照。

**明确延后**：逐像素光照/lightmap、`dTransVal`/`TransList` 透明遮挡、地牢 DUN object layer
（箱子/火把等 `objects/*.cel`）、`automap.dun`（可后续仅用于 SOL）、Hellfire `nlevels`
其它层（L5/L6 随机生成，只有 `uberroom.dun`/`cornerstone.dun` 固定房间）。

### 阶段 5：GUI 布局与显示（目标 4）
- [x] **场景查看器（前端 `web/`，2026-11-09）**：新增第 5 种浏览模式，调用 `scene.list` /
      `scene.build` 复原完整关卡场景（初始场景 Tristram 城镇），支持缩放（1–8×）/ 平移 /
      SOL 叠加层（网格 + Solid/透明/BlockLight 着色）/ 夜景提亮 / 复位。详见 `web/README.md`
      与 `docs/ARCHITECTURE.md` §10.5。
- [ ] 还原 `DiabloUI/` 的全部界面布局与元素（主菜单 / 角色面板 / 血蓝球 / 商店 / 对话框等）。
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
