# 后端 / MCP 架构文档（ARCHITECTURE）

> 状态：**架构已确认；阶段 0–1 已落地（2026-11-07）**。
> 本文档描述经用户确认的目标架构。实现顺序见文末 §9 与 `PROJECT.md` 第 8 节阶段计划。
> 当前 StormLib 引擎（vendored 于 `3rdparty/StormLib`）已验证可用，是本协议架构的“引擎底座”；
> 阶段 1 解码器（`src/decoders/{palette,clx,cel,cl2,pcx}` + `sprite_tools`）已实现并通过
> 单元 + 端到端验证，前端可经 `sprite.decode` 拿到 RGBA 帧绘制精灵。
> 阶段 4 地图管线（TIL/MIN/DUN/SOL + level CEL 重编码 + 等距合成，`src/decoders/level_*.cpp`
> + `src/tools/map_tools.cpp` 的 `map.build`）已于 2026-11-08 落地并配齐单元/集成测试。
> 2026-11-09：修正索引/背景/两遍绘制（消除「黑块」），新增 `scene.*` 完整场景复原工具与
> 前端第 5 种「场景查看器」模式（§9.3 / §10.5）。同日补齐 **静态 dLight 光照**
> （`src/decoders/level_light.cpp`）与 **A\* 寻路**（`level_path.cpp` + `scene.path`），
> 场景目录扩到 Tristram + L1/L2/L3/L4 共 6 个预设。

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
│   ├── archive.h/.cpp   #   open/list/extract/verify/querySize + 路径/档案内名归一化（`/`→`\`）
│   └── storm_wrap.*     #   StormLib 句柄管理、错误码->字符串
│
├── decoders/            # 格式解码器（移植 DevilutionX 的 load_*）
│   ├── palette.*        #   .pal  256 色调色板 -> RGBA[256]
│   ├── cel.*            #   .cel  RLE + 调色板索引 -> RGBA 帧缓冲
│   ├── cl2.*            #   .cl2 编译型动画 -> 帧组（含 cl2_to_clx 思路）
│   ├── clx.*            #   .clx DevilutionX 内部格式
│   ├── pcx.*            #   .pcx PC Paintbrush 静态图
│   ├── deflate.*        #   compressZlib/decompressZlib：RFC1951 fixed-Huffman+贪心LZ77，无 -lz 依赖
│   ├── png.*            #   encodePNG（内存）+ writePNG（文件），IDAT 调 deflate::compressZlib
│   ├── level.h          #   关卡数据结构 + kNoMicro/MapRenderOptions/SceneProjection
│   ├── level_decode.cpp #   loadTil/loadMin/loadDun/loadSol + buildMicroGrid（0-based, DRLG_LPass3）
│   ├── level_cel.cpp    #   reencodeLevelCel + decodeReencodedFrame[As]（Square/三角/梯形）
│   ├── level_render.cpp #   renderMicroTile/renderMap（两遍等距合成 + 不透明背景 + 空格预填 + 逐格 dLight）
│   ├── level_scene.*    #   SceneSpec/SceneGrid + buildTownScene/buildDungeonScene + sceneCatalog
│   ├── level_light.*    #   LightTables(Normal/Hell/Soft) + falloff/cone + buildLightMap（DoLighting）
│   ├── level_path.*     #   8 方向 A*（SOL Solid 网格，100/101 步长，scene.path 后端）
│   ├── dungeon_gen.h    #   DiabloRng（Borland LCG）+ DunTileLayer + CathedralOptions
│   ├── dungeon_gen.cpp  #   GenerateRnd/FlipCoin/discard + .dun tile 层解析
│   └── dungeon_gen_l1.cpp # drlg_l1.cpp Cathedral 逐函数移植（L1–L4 原生生成）
│
├── tools/               # 工具注册表：把 engine + decoders 组合成"工具"
│   ├── registry.h/.cpp  #   工具注册、按名分发、参数 schema、返回 JSON
│   ├── mpq_tools.*      #   mpq.list / mpq.extract / mpq.verify / mpq.recover / mpq.dump
│   ├── sprite_tools.*   #   sprite.decode / palette.load
│   ├── map_tools.*      #   map.build（阶段 4，已落地）
│   ├── scene_tools.*    #   scene.list / scene.build / scene.path（完整场景 + 光照 + 寻路）
│   ├── level_tools.*    #   level.inspect（tile 查看器）+ level.generate（Cathedral L1–L4 原生生成）
│   ├── ui_layout.h      #   （header-only 纯数据）18 屏 DiabloUI 布局目录 + 校验/查询
│   ├── ui_render.h      #   （header-only 纯像素）RGBA 画布 / blit / 元素框叠加层光栅化
│   └── ui_tools.*       #   ui.list / ui.layout / ui.build（真实 MPQ 美术合成界面）
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
| `sprite.decode`| `{archive,name,archives?,palette?,frame?,width?}` | `{name,format,width,height,frame_count,palette_resolved,frames:[{index,width,height,rgba_base64}]}`（`.clx` 直解；`.cel`/`.cl2` 经 `celToClx`/`cl2ToClx` 转 CLX 后解；`.pcx` 走内置解码。格式由扩展名自动探测。`width` 为 `.cel`/`.cl2` 必需（整数或 `"auto"` 按路径自动查表）；新增可选 `widths`（逐帧宽度整数数组，仅 `.cel`，优先于 `width`）——`data\inv\objcurs.cel`(179 帧)/`objcurs2.cel`(61 帧) 每帧宽度不同，`width:"auto"` 会按名套用 `src/decoders/inv_widths.h` 的内置逐帧表（转录 DevilutionX `objcurs*-widths.txt`），无需前端传大数组。`archives` 为额外 MPQ 路径数组，精灵/调色板可来自与 `archive` 不同的 MPQ，前端据此跨档案解析调色板） | ✅ |
| `sprite.list`  | `{dict?,devilutionx_root?,family?,class?,kind?}` | `{count,archives:[path],sprites:[{name,archive,archive_path,family,class,type,width}]}`（从 `recovered.json` + DevilutionX 数据表枚举**真实存在的**精灵名并分类 family/class/type、带自动帧宽，供前端建目录树；`archive_path` 直接取自 recovered.json 的档案 `path`，前端无需再手动填 MPQ 路径；`archives` 返回 recovered.json **全部** MPQ 路径（含无 CL2/CEL 精灵的 `devilutionx.mpq`），前端整栈传给 `ui.build`/`scene.build`，否则 `.clx` 界面美术报 `missing`、依赖 `ui_art\diablo.pal` 的 CLX 报 `decode_error`；`kind` 默认 `sprite`（CL2/CEL），可传 `palette` 仅列 `.pal` 调色板、`all` 两者都列（用于前端调色板下拉框），或 `ui` 仅列界面布局转录的美术（`ui_art\*.pcx`、`data\*.clx/.cel`、`ctrlpan\*.cel` …，每条带 `palette`/`width`/`frames`/`transparent_index`，`family:"ui"`，`archive_path` 由 archive_hint 解析）；前端 `loadCatalog()` 额外拉 `kind:"ui"` 合并进目录树，使 UI 美术在目录浏览里也能查看及播放帧动画；无 listfile 时 `mpq.list` 给的是占位名，故需此工具） | ✅ |
| `spell.list`   | `{devilutionx_root?,source?,id?}` | `{count,spell_count,monster_attack_count,ability_count,trap_count,cast_action_map,impact_catalog[],spells[],monster_attacks[],abilities[],traps[]}`（从 `spelldat`/`misdat`/`missile_sprites`/`monstdat` 四表 join 出**玩家技能与怪物远程攻击的有序阶段目录**：`stages[]` 固定 `cast→projectile→impact→debuff`；每层 `L={kind,role,anchor,confidence,...}`，`sprite` 层带编号 `missiles\{name}{N}.cl2` 文件清单 + 显式 `width` + `frames`，`trn` 层带 `trn:{name,preset,apply_to}`；`impact_catalog` 为候选命中特效。**每条 `spells[]`/`monster_attacks[]` 额外带 `name_zh`（中文名，取自 `Translations/zh_TW.po` → `zh_CN.po` 的 `msgctxt "spell"`，无翻译则回退英文名）、`description`（按已解析的阶段链 + 玩家法力/智力消耗合成的中文说明），以及**法术级别参数**：`level`（`spelldat.bookLevel` 法术阶）、`staff_level`、`mana_cost`、`mana_multiplier`（每升 1 级训练减的法力）、`min_mana`、与 `mana_by_level`（按 `GetManaAmount` 公式算出的 1..bookLevel 各级基础法力 `{level, mana}`，含 `min_mana` 下限、未乘职业系数）；另含 `visual_by_level`（玩家 1..bookLevel / 怪物 1..16 各级视觉参数：持续时间 `duration_ticks`/`duration_sec`、弹速 `speed`、范围 `range_tiles`/`radius_tiles`、弹射物数 `count` 及法术特有字段如 `segments`/`wall_length_tiles`，含角色/地下城层依赖项标 `_note`，无级别缩放为空；键因各法术硬编码公式而不同，由 `spell_visual_model.h` 逐法术转录）；**怪物攻击除 `GetMissileType()` 的 14 类 AI 外**，还包含**在 AI 函数内直接 `AddMissile` 的形态**（`monsterAttackForms()`，转录自 `CounselorAi`/`BatAi`/`MegaAi`：Counselor 按智力 0..3 → Firebolt/ChargedBolt/LightningControl/Fireball，外加近身 Flash 双层；Familiar → Lightning；Gloom → Rhino 冲撞；Mega → InfernoControl）。这些形态各自独立成条（`id` = `AI` 或 `AI.variant`），并带 `variant`（形态名）与 `condition`（中文触发条件），`monsters` 按智力/怪物 id 精确收窄。**`unique_monstdat.tsv` 会作为额外怪物行合并**（唯一怪的 `ai`/`intelligence` 覆盖基础怪），因此 Hork Demon（唯一怪 `ai=HorkDemon`）的 `HorkSpawn`、Zhar/Lazarus（`ZharAi`/`LazarusAi` 委托 `CounselorAi`，`normalizedMonsterAi()` 归一为 Counselor）以及 Red Vex/Black Jade（`LazarusSuccubus`）都能正确归属。`abilities[]` 另列**引擎级怪物特殊能力**（`monsterAbilities()`：`Blink` 受重击瞬移、`AcidPuddle` 酸液怪死亡留池、`Hidden` 潜行系隐身），每条带 `monsters`/`trigger`/`source_ref`；Diablo 1 / Hellfire **没有** Diablo 2 式词缀（Thunderstorm / Waller / Vortex / Shielding 在引擎中不存在）。`traps[]` 列**陷阱法术**（`trapSpecs()`，转录自 `objects.cpp`）：`OBJ_TRAPL/R` 墙陷阱按 `GenerateRnd((effLevel/3)+1)` 依次解锁 Arrow / Firebolt(≥3 层) / LightningControl(≥6 层)；`OBJ_TCHEST1..3` 带陷阱宝箱按类型 0..2 发 Arrow/FireArrow/Nova，地狱火扩到 0..5 追加 RingOfFire/StealPotions/StealMana；地狱火的 `OBJ_FLAMEHOLE`/`OBJ_FLAMELVR` 喷火口与火焰拉杆为对象火焰动画 + 直接伤害（伤害类型记 `FireWallControl`，无 missile 图形）。每条 `traps[]` 带 `objects`（宿主 `OBJ_*`）、`trigger`、`source_ref`。当前实测 `spell_count=51`、`monster_attack_count=25`、`ability_count=3`、`trap_count=10`） | ✅ |
| `map.build`    | `{archive,cel,til,min,sol,dun,palette?,archives?,micro_len?,blocks?,out?}`（L1 例：`levels/l1data/l1.cel/til/min/sol` + `levels/l1data/rnd2.dun`） | `{image_path,preview_path,meta_path,preview_base64,width,height,grid_width,grid_height,micro_len,frame_count,micro_count,solid_count,transparent_count}`；写完整地图 PNG + ≤400px 预览 PNG + 每格标志 sidecar JSON 到 `out/maps/`；`meta` 含 `solid`/`transparent`/`block_light`/`block_missile` 四个 `grid_width×grid_height` 数组 | ✅ |
| `scene.list`   | `{}` | `{count,scenes:[{id,title,layout,grid_width,grid_height,micro_len,blocks,ambient_light,light_style}]}`；内置完整场景目录（Tristram + L1/L2/L3/L4 共 6 个预设），无需 MPQ | ✅ |
| `scene.build`  | `{id,archive,archives?,out?,preview_max_width?,lighting?,ambient?,lights?,auto_light?}` | 复原**完整场景**（两遍绘制 + 不透明背景 + 预填，0 透明像素；可选 dLight 光照）。返回 `{image_path,preview_path,overlay_path,meta_path,image_base64,overlay_base64,width,height,preview_width,preview_height,scale,grid_width,grid_height,micro_len,frame_count,solid_count,walkable_count,transparent_count,transparent_pixel_count,lighting,ambient,light_lit_count,meta}`；`meta` 含 `solid`/`transparent`/`block_light`/`block_missile`/`walkable` + 可选 `light`；原生 PNG 落盘到 `out/scenes/`，WS 只传 ≤1000px 预览 + 对齐叠加层 | ✅ |
| `scene.path`   | `{id,archive,archives?,from:{x,y},to:{x,y},max_steps?,preview_max_width?}` | `{found,cost,steps,grid_width,grid_height,micro_len,walkable_count,scale,native_width,native_height,preview_width,preview_height,path:[{x,y,screen_x,screen_y,preview_x,preview_y}]}`；SOL `Solid` 网格 8 方向 A*（直行 100 / 斜行 101），坐标与 `scene.build` 投影对齐 | ✅ |
| `level.inspect`| `{archive,archives?,cel,til,min,palette,sol?,blocks,micro_len,megatile?/micros[]}` | `{megatile,grid_width,grid_height,micros:[{id,blocks:[{mt,data,has_value,type,type_name,frame}],width,height,png_base64}],frames:[{frame,type,type_name,width,height,png_base64}],assembly:{width,height,native_width,native_height,left,top,png_base64}}`；按 TIL→MIN→level CEL 逐块查看并给出等距组装预览（关卡 tile 查看器后端） | ✅ |
| `level.generate`| `{archive,archives?,level,seed,original_cathedral?,quests?,out?}` | `{level,seed,quests,original_cathedral,width,height,dun_path,floor_count,dirt_count,stairs_up_count,stairs_down_count,tiles:[40 行 hex],note}`；Cathedral L1–L4 按 seed 原生生成（DevilutionX `drlg_l1` 逐函数移植），写 ExportDun 兼容 `.dun`，可交给 `map.build` 的 `dun_path` 渲染；14 个 DevilutionX fixture 0 差异 | ✅ |
| `ui.list`       | `{category?}` | `{count,canvas_default,categories,screens:[{id,title,category,canvas,element_count,text_element_count,art_ref_count,panel_count,palette,needs_width,source_ref}]}`；18 屏 DiabloUI 布局静态目录（menu/hud/panel/store/dialog），无需 MPQ | ✅ |
| `ui.layout`     | `{id}` | 单屏完整布局：`{id,title,category,canvas,palette,panels[],elements[],art_refs[],needs_width,overlay_box_count,source_ref}`；每元素 `{index,id,type,label,rect:[x,y,w,h],panel,panel_rect,z,art?,grid?,text?,font_size,color,align,flags,source_ref}`。文字不烧录，驱动前端近似字体层。无需 MPQ | ✅ |
| `ui.build`      | `{id,archive,archives?,out?,palette?,overlay?,label_numbers?,background?,skip_missing_art?}` | 真实 MPQ 美术离屏合成 640×480 PNG（不透明背景、0 透明像素）+ 对齐元素框叠加层。`{image_path,overlay_path,meta_path,image_base64,overlay_base64,width,height,element_count,overlay_box_count,elements[](+decode_status/decoded_size/placed_rect),art[](status/width/height/bytes),needs_width,palettes[],palette,art_ok/missing/error_count,transparent_pixel_count,meta}`；写 `out/ui/<id>.png` + `_overlay.png` + `.json`。逐资源 `ArtRef.palette`（菜单/CLX 用 `ui_art\diablo.pal`、游戏内 CEL 用 `levels\towndata\town.pal`），见 §11 | ✅ |

> **PCX 已并入 `sprite.decode`**（按 `.pcx` 扩展名自动路由），不再单独设 `pcx.decode` 工具。
> `sprite.decode` 的调色板解析优先级：`palette`（base64 或名称）→ 文件内嵌（PCX）→ 同目录
> `<base>.pal`；都找不到则报错。返回中 `palette_resolved` 标记是否成功解析到调色板。
> **跨 MPQ 调色板**：精灵与其调色板常不在同一档案（如 DIABDAT 的 `missiles\fireplar.cl2` 配
> hellfire 的 `nlevels\cutl5.pal`）。前端把「精灵所在 MPQ + 调色板所在 MPQ」一并经 `archives`
> 传入，`sprite.decode` 在整组档案栈中查找；缺省只查 `archive`（旧行为，跨档案会报
> `extract failed (name not found?)`）。
> 工具名用点分命名空间，便于 Agent 理解分组，也方便前端按前缀路由 UI。

> **`spell.list` 与编号特效文件**：`recovered.json` 只恢复了 33 个单帧 `missiles\` 名，
> 火球 / 火墙 / 圣光等核心特效是 `missiles\{name}{N}.cl2`（`numFrames` 个编号文件，
> 各含 `frameLength[i]` 帧），不在其中。四表 join 链路：
> `spelldat.missiles`(MissileID) → `misdat.id→graphic/flags` →
> `missile_sprites.id→name/width/numFrames` → 文件清单。
> 编号文件的 `width` 必须显式传（`sprite_width.cpp` 按 basename 精确匹配，
> `fireba1` 查不到表里的 `fireba`）。怪物远程攻击由
> `Source/monster.cpp::GetMissileType(ai)` 的 switch 转录（后端内置常量表）驱动。
> 命中/爆炸阶段无数据表外键，按命名约定自动关联 + 少量 override，
> 其余进 `impact_catalog` 标 `confidence:"catalog"`。debuff 的 TRN
> （`infra.trn` / `stone.trn`）本是玩家调色板重映射，套怪物层为**近似** tint。
> 纯 join 逻辑在 `src/tools/spell_tables.h`（header-only，不依赖 json，便于单测）。

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
6. **枚举精灵（工具 `sprite.list`，已落地）**：
   ```json
   {"jsonrpc":"2.0","id":2,"method":"tools/call",
    "params":{"name":"sprite.list","arguments":{"family":"plrgfx","class":"warrior"}}}
   // → result.sprites[] 含每件精灵的 name / width / type，前端据此建 职业×护甲×武器×动作 树
   ```
7. **自动取宽（`sprite.decode` 已支持 `width:"auto"`）**：
   ```json
   {"jsonrpc":"2.0","id":3,"method":"tools/call",
    "params":{"name":"sprite.decode",
              "arguments":{"archive":"DIABDAT.MPQ",
                           "name":"plrgfx\\warrior\\wlu\\wluas.cl2",
                           "width":"auto"}}}
   // width:"auto" 由 Server 按路径查 DevilutionX 数据表取宽，前端无需自带宽度表
   ```
   `autoWidth` 覆盖 `plrgfx`/`monsters`/`missiles`/`objects`/`towners` 五张 TSV 表，
   外加 `items`：物品 CEL 无 TSV，按 DevilutionX `LoadCel` 编译期宽度解析——掉落动画
   统一 `ItemAnimWidth=96`（`items.h:185`）、`items\duricons.cel`=32
   （`control_panel.cpp:406`）、`items\map\mapztown.cel`=640（`doom.cpp:25`），
   因此 `sprite.list` 对 `items` 条目也返回可用 `width`，目录浏览可直接解码。
   `data\inv\` 另立 `inv` 家族：物品栏光标图标 `objcurs.cel`(179 帧)/`objcurs2.cel`(61 帧)
   **逐帧变宽**，单一 `width` 无法解码，`width:"auto"` 改走 `src/decoders/inv_widths.h`
   的内置逐帧宽度表（转录 DevilutionX `Source/cursor.cpp:445-451` 所用的
   `objcurs*-widths.txt`），也可显式传 `widths:[…]`；`celToClx` 新增逐帧宽度重载，
   帧循环按全局帧索引取宽。物品栏背景 `inv*.cel` 仍是单帧固定 `width=320`
   （`SidePanelSize`，`control.hpp:38`），`sprite.list` 直接给出该宽度。
   `sprite.list` **不**在条目里携带 `widths` 数组（179×跨档案重复会撑大 WS 载荷）。
  再立 **`level` 家族**：`levels\l<d>data\l<d>.cel` 与 `nlevels\l<d>data\l<d>.cel`
  是**关卡瓦片集（非帧精灵）**，不能走 `sprite.decode`（报 `cel requires a width`）。
  `classifySprite` 把它们归到 `level` 家族、`class` 为 `L1..L6`、条目带 `tileset`
  （`l1..l6`，对应 `web/src/level.js` 的 `TILESETS`）；`sprite.list` 对 `level` 家族
  `width` 固定 0。前端点击时（见 `web/src/main.js` 的 `onSelectSprite`）不调用
  `sprite.decode`，而是 `window.open("level.html?tileset=lN")` 跳到关卡查看器，由
  `level.inspect` 渲染 TIL→MIN→CEL（含正确调色板变体 `l1_2.pal`/`l3pfoul.pal`/`l5base.pal`…）。

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
   - **`cl2ToClx` 输入校验**（2026-11-08）：CL2 里的所有偏移均来自文件、属不可信输入，
     `src/decoders/cl2.cpp` 对每个偏移做 `[data, data+size)` 边界校验，并把帧解码循环的
     `while (src != frameEnd)` 改为 `src < frameEnd`，读到坏数据即抛 `std::runtime_error`
     （工具层转成干净的 JSON-RPC error）。修复前 96 字节残缺 CL2
     （`monsters\darkmage\dmagew.cl2`，DIABDAT.MPQ 内的 stub）会让 `src` 越过缓冲区触发
     SIGSEGV，连带同批 stdio 请求全部丢失；修复后返回 `invalid CL2: ...`，全量 325 条
     回归 `NO RESPONSE` 由 15 降为 0。
   - **`celToClx` 输入校验**（2026-11-08）：`src/decoders/cel.cpp` 同样加 `[data, data+size)`
     偏移校验、`src < srcEnd`、以及 `int remaining`（原 `unsigned` 在超宽 run 时下溢空转），
     错误 `frameWidth`（如 `cow.cel` 传 64/96/160）不再 SIGSEGV。
   - **目录合成修数据**（2026-11-08）：`synthesizeMonsters` 排除 `worm\worm`（beta 占位、
     MPQ 无 CL2；不按 `availability` 过滤以免误删 golem/unrav 等 15 个真实 Never 条目）；
     `synthesizeTowners` 从 `TOWN_COW` 行取 `animWidth=128` 填入硬编码 cow，
     `autoWidth` 增加 `TOWN_COW` 回退使 `cow.cel` 的 `width:"auto"` 可解码。
   - **单元验证**（三套件，均 PASS，见 `README.md` §测试）：
     - `tests/unit_tests.cpp`：CLX 解码、CEL→CLX / CL2→CLX 往返、**CL2/CEL 畸形输入与错误
       width 拒绝**（`testCl2RejectsMalformed` / `testCelRejectsMalformed`）、PCX 内嵌调色板、
       调色板加载、base64 编解码（无需 MPQ）。
     - `tests/core_tests.cpp`：`ToolRegistry` 注册/重名/分发/异常/`list`，`handleRequest`
       的 `tools/list`/`tools/call` 与各错误码（无需 MPQ）。
     - `tests/integration_tests.cpp`：`engine::Archive` 全链路（open/list/verify/querySize/
       extract/extractToFile）+ `mpq.list`/`mpq.verify`/`mpq.extract`/`mpq.dump`/`mpq.recover`
       （需真实 MPQ，见 `DHFC_TEST_MPQ`；缺失则 SKIP）。
   - **端到端验证**：`palette.load` 对 `devilutionx.mpq` 的 `gendata\cut2.pal` 返回 256 色
     + 合法 RGBA base64；`sprite.decode` 对 `devilutionx.mpq` 的 `data\health.clx` +
     `ui_art\diablo.pal` 返回正确 264×19 RGBA 帧。
8. **地图（阶段 4，已完成 2026-11-08，2026-11-09 补光照/寻路）**：`src/decoders/level_*.cpp`
   + `src/tools/map_tools.cpp` 的 `map.build`（在 `main.cpp` 调 `registerMapTools(reg)`，
   stdio + WS 双传输复用同一套 handler），以及 `level_light.cpp` / `level_path.cpp`
   + `scene_tools.cpp` 的 `scene.list`/`scene.build`/`scene.path`。详见 §9.2/§9.3。

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

### 9.2 关卡地图管线（阶段 4，`map.build`）

忠实移植 DevilutionX `Source/levels/` 的拼装与 `Source/engine/render/scrollrt.cpp` 的等距合成，
后端纯 C/C++，无 Node 运行时。全链路（2026-11-09 修正版）：

```
MPQ 抽取
  levels/l1data/l1.til ─► loadTil    ─► megatiles[]  (每 8B = 4 个 micro1..micro4, uint16 LE)
  levels/l1data/l1.min ─► loadMin    ─► micros[]     (每 piece 10/16 个 LevelCelBlock,
                                                    data 低12位=1-based帧号, 高3位=TileType)
  levels/l1data/*.dun  ─► loadDun    ─► DunFile     (width/height + tileLayer + 可选透明层)
  levels/l1data/l1.sol ─► loadSol    ─► solFlags[]  (按微瓦片 piece id 索引, 零填充到 MAXTILES)
  gendata/cut2.pal     ─► loadPalette/paletteToRGBA ─► RGBA[256] (1024B)
  levels/l1data/l1.cel ──┐
                         ├─► buildMicroGrid(dun, megatiles) → (2w)×(2h) 0-based 微瓦片索引网格
                         │     (DRLG_LPass3：兆瓦片展开为 2×2；kNoMicro=界外；renderMap 读 micros[mi])
                         ├─► buildFrameTypes(micros, microLen) → 每帧 TileType
                         └─► reencodeLevelCel(cel, frameTypes) → 定长帧缓冲
                               (Square 1024B / 三角 512B / 梯形 784B / 透明方保留 RLE)
  renderMap(reencoded, micros, grid, gw, gh, solFlags, transparency, palRGBA, microLen, opts)
       ├─ Pass 1 地板：每格 mt[0]/mt[1] 强制左/右三角 → 64×32 菱形（铺满地面）
       ├─ Pass 2 墙体：升序 i+j, mt[2..] 成对向上叠, hasValue() 守卫
       ├─ 背景：每像素先填 opts.background（不透明）→ 0 透明像素，无「黑块」
       ├─ 光照（可选 opts.lightLevels）：每格用 LightTables[level] 烘焙调色板解码
       └─► MapRenderResult: rgba 全图 + solid/blockLight/blockMissile/transparent/light 每格数组
```

**TileType**（`dun_tile.hpp`）：Square(0)、TransparentSquare(1)、LeftTriangle(2)、
RightTriangle(3)、LeftTrapezoid(4)、RightTrapezoid(5)。新增 `decodeReencodedFrameAs`
支持强制三角形（地板，`DrawFloorTile` 语义）。`sceneProjection(gw,gh,microLen)` 抽出包围盒，
供渲染器与叠加层共用同一坐标。

**等距变换**（DevilutionX 屏幕轴）：`screenX=(i-j)*32`、`screenY=(i+j)*16`，
`screenY` 越大越靠近观察者；**绘制顺序为升序 `i+j`**（从远到近）。
（2026-11-09 修正：此前用 `(j-i)` / `-(i+j)`，相对游戏原图整体旋转 180°。）

**关键约定 / 已踩坑（2026-11-09 更正）**：
- **TIL 微瓦片索引是 0-based 原样值**（`micros[mi]`），`0` 是合法地板；`LevelCelBlock.frame()`
  的 1-based 是 CEL 帧号，二者**不可混淆**。初版误用 `micros[mi-1]` 并把 `mi==0` 当空跳过，
  是黑块/空洞的根因之一。真界外用 `kNoMicro=0xFFFF` 哨兵。
- **场景没有空格**：DevilutionX 预填整个 `dPiece`（城镇 426 / 地牢 Dirt），本实现用
  `opts.prefillMicro` 复现；背景改为不透明填充，输出 0 透明像素。
- **SOL 按微瓦片 piece id 索引**（`SOLData[dPiece[x][y]]`），不是兆瓦片 id；磁盘文件仅
  `pieceCount` 字节（l1=453 / town=1258），读入 MAXTILES 零填充表。
- `paletteToRGBA` 产出 **1024 字节 RGBA**（4 字节/项），`CopyRun` 必须按 4 字节步长索引。
- StormLib 档案内名必须是 **反斜杠 `\`**；`Archive::normalizeArchiveName()`（`archive.cpp`）
  在 `extract`/`verify`/`querySize`/`extractToFile` 内部统一把 `/` 转成 `\`，所有工具受益。

**`map.build` 产出**（`out/maps/<sanitized-dun>.*`）：完整地图 PNG、≤400px 预览 PNG、
sidecar JSON（`grid_width/grid_height/micro_len/frame_count` + 四个每格标志数组）、
返回摘要与 base64 预览。**`scene.build` 产出**（`out/scenes/<id>.*`）：原生 PNG + 整数
降采样预览 PNG + 对齐的叠加层 PNG + sidecar JSON，返回 `image_base64`/`overlay_base64`、
`preview_width/height`、`scale`、`meta`（每格标志 + `walkable` + 可选 `light` +
`transparent_pixel_count`）。`map.build` 固定 FullyLit；`scene.build` 默认 FullyLit，
`lighting:true` 时应用静态 dLight（见下）。

**静态光照（`level_light.cpp`，2026-11-09）**：忠实移植 `MakeLightTable` 的 16 级
`LightTables`（Normal / Hell / Soft 三套色阶）与 `DoLighting` 的 `LightFalloffs` /
`LightConeInterpolations`。`buildLightMap(gw,gh,ambient,lights,soft)` 生成每格 0..15
（0 全亮、15 全黑）；`renderMap` 对每格选择 `LightTables[level]` 烘焙后的 RGBA palette 解码
瓦片，因此暗部是**调色板索引重映射**而非 RGB 近似。`scene.build` 参数：
`lighting`（默认 false）、`ambient`（默认城镇 0 / 地牢 15）、`lights[]`
（`{x,y,radius,offset_x,offset_y}` 微瓦片坐标）、`auto_light`（无光源时中心 radius-15 火把）。
`meta.light` 输出逐格级别；`light_lit_count` 为 level 0 格数。

**寻路（`level_path.cpp` + `scene.path`，2026-11-09）**：在 SOL `Solid` 网格上做 8 方向 A*：
直行代价 100、斜行 101（照搬 `Source/engine/path.cpp`），斜向不额外限制转角（`CanStep`
对斜向返回 true），目标格允许 Solid（同 `FindPath` 对 destination 的放宽）。`scene.path`
输入微瓦片 `from`/`to`，返回 `found`/`cost`/`steps`/`walkable_count` 与 `path[]`（每格
`x/y`、原生 `screen_x/screen_y`、按 `preview_max_width` 换算的 `preview_x/preview_y`），
坐标与 `scene.build` 的 `SceneProjection` 完全一致，可直接叠图。

**动态地图**：L1–L4 / L5–L6 是运行时按 `DungeonSeeds[level]` 生成的，MPQ 中没有整层 `.dun`。
规则、LCG、各生成器流程与 DevilutionX 调试版 `dev.level.exportDun()` 工作流见
**`docs/MAP_GENERATION.md`**；`map.build` 新增 `dun_path` 可直接渲染导出的 40×40 `.dun`。
**Cathedral L1–L4 已可原生生成**：`level.generate`（`src/decoders/dungeon_gen*.cpp`）按 seed
输出 ExportDun 兼容 `.dun`，与 DevilutionX 14 个 Cathedral fixture 逐格 0 差异；
Catacombs/Caves/Hell/Crypt 仍待移植。

**测试目录**（2026-11-12 调整）：三套测试源码（`tests/unit_tests.cpp` /
`tests/core_tests.cpp` / `tests/integration_tests.cpp`）统一置于 `tests/`；`out/` 整目录被
`.gitignore` 忽略，只承载 mcp-server 与 `mpq-info/` 辅助工具的运行产物。测试内硬编码的运行
临时产物路径（`tests/__png_test.png`、`tests/_it_*`、`tests/gen_test/`、`tests/ui/`）及
编译出的 `tests/*.exe` 均由 `.gitignore` 忽略。三套测试均为自包含：不读取任何 JSON 夹具，
原置于 `tests/` 的 21 个手工 JSON-RPC 请求模板/响应快照已移除，移除后三套仍全绿。三套
g++ 构建/运行命令见 `README.md` §测试。

**测试**（见 `README.md` §测试 / §6）：
- `tests/unit_tests.cpp`：TIL/MIN/DUN/SOL、`buildMicroGrid` 0-based、Square/Triangle CEL、
  强制三角形、`encodePNG`、`renderMap` 两遍绘制 + 不透明背景 + 0 透明像素 + 空格预填 +
  frame()==0 守卫、**`level_light` 光照表/falloff/cone/buildLightMap 与逐格着色**、
  **`level_path` A\*（墙体/斜角/Solid 目标/maxSteps）**、`buildTownScene`/`buildDungeonScene`、
  `sceneProjection`、**`testDeflateRoundTrip`**（`compressZlib`↔`decompressZlib` 回环 + 5 组代表数据 +
  length 3–258 × distance 全 30 码边界 15 万+ 组合，覆盖 RFC 1951 额外位边界），
  PNG write round-trip 改用 `decompressZlib` 解压 IDAT、**`testDungeonGen`**（LCG/GenerateRnd、
  DUN 解析、任务默认值、确定性/结构、缺失 set-piece 报错 + 14 个 DevilutionX Cathedral
  fixture 的 40×40 全格 0 差异 oracle；fixture 缺失时 SKIP）。
- `tests/integration_tests.cpp`：`testLevelMap`（真实 L1，断言 0 透明像素）、
  `testLevelGenerate`（`level.generate` 从 MPQ 生成 Cathedral L2，断言 40×40 hex /
  54404 字节 ExportDun 文件 / `loadDun` 识别 / 同 seed 确定性）、
  `testSceneTools`（真实 Tristram，断言 0 透明像素 + PNG/叠加层/标志/`walkable` 数组 +
  `scene.path` 起止与坐标；`l1_rnd2` lighting、`l4_diab1` 12/16 瓦片）。

### 9.3 场景管线（阶段 4/5，`scene.*` + 前端场景查看器）

`map.build` 渲染的是**孤立 `.dun` 预设**；`scene.*` 复原的是引擎里真正的**完整场景**。
模型与产出详见 `docs/PROJECT.md`「游戏场景如何组织」一节，此处只列工具与数据流：

```
scene.list  ─► 静态目录（tristram / l1_rnd2 / l1_skngdo / l2_blind / l3_anvil / l4_diab1）
scene.build {id,archive,archives?,out?,preview_max_width?=1000,lighting?,ambient?,lights?,auto_light?}
   ├─ 抽取: town.{cel,til,min,sol,pal} + sector1s..4s.dun（城镇）
   │        或 lN.{cel,til,min,sol} + *.dun + lN palette（地牢 L1..L4）
   ├─ 组装: buildTownScene（预填 426 + 4 sector, DUN word0→218, 96×96）
   │        或 buildDungeonScene（L1..L4 prefill 22/12/8/30 + DUN@16,16；
   │          DUN word0→Floor 13/3/7/6；Pass3→InitDungeonPieces 标 dSpecial + l1s/l2s CEL）
   ├─ 光照: buildLightMap(ambient + lights) → LightTables 烘焙每格 palette（可选）
   ├─ 渲染: buildFrameTypes → reencodeLevelCel → renderMap(两遍, 不透明背景, prefill, dLight)
   ├─ 叠加层: buildOverlay（sceneProjection 同坐标；主要网格 + SOL 着色，preview 分辨率）
   └─ 产出: encodePNG 原生/预览/叠加 → 文件 + base64；
            meta{solid[],transparent[],block_light[],block_missile[],walkable[],light[]?,transparent_pixel_count}

scene.path {id,archive,archives?,from:{x,y},to:{x,y},max_steps?=1024,preview_max_width?=1000}
   └─ loadSceneAssets → SOL solid[] → findPath(A*,100/101) → path[]{x,y,screen_*,preview_*}
```

- 原生 PNG 只落盘（城镇 96×96 网格约 6145×3296），**WS 只传整数降采样后的
  ≤1000px 预览 + 同尺寸叠加层**，二者由同一 `SceneProjection` + 同一 `step` 生成，
  像素级对齐。`src/decoders/png.cpp::encodePNG` 提供内存 PNG 字节（供 base64）。
- **tiles / objects**：`renderMap` Pass 1 只处理 `IsFloor`（SOL 非 Solid/BlockMissile）格子，
  避免对墙基/树叶数据强行三角形解码；`TransparentSquare` 地板按真实 RLE 解码
  （DevilutionX 的 `ReencodeDungeonCels` 会拆成 triangle floor + foliage，本实现直接画完整
  RLE，先保证不产生噪点）。`dSpecial` 映射到独立 special CEL、在 `Meta.special` 暴露并
  按底部锚定绘制：城镇 `town.cpp::InitTownPieces` → `levels/towndata/towns.cel`（64×224
  树冠帧）；地牢 `drlg_l1.cpp` / `drlg_l2.cpp::InitDungeonPieces` → `levels/l1data/l1s.cel`
  / `levels/l2data/l2s.cel`（64px 拱门与门框，8 帧）。Caves / Hell 的 `Pass3()` 没有
  `InitDungeonPieces()`，故 `l3_anvil` / `l4_diab1` 不加载 special CEL。
  **地牢 DUN 值为 0 的格**按 `LoadDungeonBase → PlaceDunTiles(dunData,{0,0},floorId)`
  铺成本层 Floor megatile（L1=13 / L2=3 / L3=7 / L4=6），而非保留 Dirt 预填——
  这些格是房间的可走地板/通道；修复前被画成实心墙（`walkable=0`）。
  Tristram 另外应用非任务态 `FillTile`（水井/喷泉、传送入口）。地牢 DUN object layer
  （`AddL1Objs` 门物件）与 `dTransVal`/`TransList` 透明遮挡尚未实现。
- **tile 纵向翻转修复（2026-11-09）**：`DecodeLeft/RightTriangle` 与
  `DecodeLeft/RightTrapezoid` 输出 top-down RGBA，而 `DecodeSquare/TransparentSquare`
  输出 bottom-up（源 CEL 行 0 为底）；`blitFrame` 又统一翻转一次，导致三角/梯形 tile
  上下颠倒（Tristram 水池的 `RightTrapezoid` 环错位、看起来不是封闭圆）。现在所有
  level CEL 解码器统一输出 top-down，`blitFrame` 不再翻转。
- **`level.inspect` + `web/level.html`**：按 megatile/micro/frame 粒度查看
  `.til`→`.min`→`.cel` 的 block 类型、帧号、原始 data、帧 PNG 与等距组装预览；
  默认预填 Tristram `megatile=71`（水池 pieces 257–260 / frames 594–605）。
- 前端 `web/` 第 5 种浏览模式（§10.5）把两张 PNG 解码为 bitmap 后绘制到叠放的两个
  `<canvas>`，用同一 transform 做缩放/平移；新增「光照」开关触发 `scene.build(lighting:true)`。
  后端 `src/main.cpp` 调 `registerSceneTools(reg)`，stdio + WS 共用 registry。

---

## 10. 前端引用后端（阶段 2：角色 / 怪物查看器）

> 目标：浏览器里的**可视化查看器**，通过 WebSocket 调用本地 `mcp-server` 的
> `sprite.*` 工具拿到 RGBA 帧并绘制，自身**不读 MPQ、不碰文件系统**。这是把
> 阶段 1 的解码能力变成"可看"的下一步，也是阶段 2/3 查看器目标的天然载体。

### 10.1 当前已具备（无需重造）

- `mcp-server` 已带 **WS 桥**（`--ws <port>`，自研 RFC6455），与 stdio 共用同一套 tool `dispatch`。
- 后端已注册 `palette.load`、`sprite.decode`、`sprite.list`（`src/tools/sprite_tools.cpp`）：
  `sprite.decode` 能从 MPQ 抽出 `.clx/.cl2/.cel/.pcx` 并解码成
  `frames:[{index,width,height,rgba_base64}]`，前端用 `ImageData` 直接画；
  `sprite.list` 从 `recovered.json` + DevilutionX 数据表枚举真实精灵名（分类 + 自动帧宽）。
- 因此"前端引用后端"的主干链路**已经打通**，缺的是让前端"好用"的少量后端补齐（TRN）+ 前端壳。

### 10.2 后端补齐状态（纯 C/C++，可独立验证）

1. **`sprite.decode` 支持 `width:"auto"`**（✅ 已完成）
   - `width` 参数为 `"auto"` 或非正时，调用共享模块 `src/decoders/sprite_width.cpp`
     的 `autoWidth()`（路径→家族→宽度表查表：plrgfx→`sprites.tsv`、monsters→`monstdat.tsv`、
     missiles→`missile_sprites.tsv`、objects→`objdat.tsv`、towners→`towners.tsv`；items 无表仍手填）。
   - 已通过 `sprite_width` 单元测试 + 手动 stdio 端到端验证（`wluas.cl2`→96、`whaat.cl2`→128 等）。
2. **新增 `sprite.list` 工具**（✅ 已完成）
   - `mpq.list` 在无 listfile 时只给占位名 `FileNNNNNNN.xxx`，前端无法据此浏览。
   - `sprite.list` 读 `mpq-info/dict/recovered.json` + DevilutionX 数据表，枚举
     **真实存在**的精灵名，并按 family/class/type 归类，返回
     `{count,sprites:[{name,archive,archive_path,family,class,type,width}]}`（width 为自动帧宽，items 为 0）。
   - 支持 `kind`：`sprite`（默认，CL2/CEL）/ `palette`（仅 `.pal`）/ `all`（sprite+palette）
     / `ui`（界面布局转录美术，每条带 `palette`/`width`/`frames`）；前端用
     `kind:"palette"` 拉全部调色板填入下拉框，并额外拉 `kind:"ui"` 合并进目录浏览树。
   - 前端据此建"职业 × 护甲 × 武器 × 动作"目录树，点击即 `sprite.decode`。
   - 已加入 `integration_tests.cpp`（合成 recovered.json 断言 sprite/palette/all 分类正确）。
3. **（可选，保真度）TRN 上色**（已实现，2026-11-07）
   - TRN = **256 字节调色板索引重映射表**，经 `ClxApplyTrans` 把每帧每个像素索引 `i`
     替换为 `trn[i]` 改变上色。`sprite.decode` 现新增 `apply_trn`（布尔，默认 false）参数：
     开启后按 `trn_name`（可选，显式指定 TRN 文件）或默认 `plrgfx\trn<class>.trn`（来自
     `sprites.tsv` 的 `trn` 字段，即 `trn`+职业，如 `trnwarrior`）取 256 字节 TRN，重映射调色板
     后再解码；返回 `trn_applied` / `trn_name` 标记是否实际作用。
   - **两类 TRN（取自 `Source/player.cpp` `LoadPlrGFX`）**：
     1. **每精灵 TRN（先应用）**：`GetPlayerGraphicTRN(pszName)` → `<spritePath>.trn`
        （`plrgfx\{class}\{prefix}\{prefix}{type}.trn`）。候选名校验命中为 **0**，恒 `nullopt`，
        对玩家精灵无实际效果。
     2. **职业 TRN（后应用）**：`GetClassTRN(player)` → `plrgfx\trn<class>.trn`
        （如 `plrgfx\trnwarrior.trn`，来自 `sprites.tsv` 的 `trn` 字段）。
     - 应用顺序固定：**先 `ClxApplyTrans(graphicTRN)` 再 `ClxApplyTrans(classTRN)`**。
   - **实测结论（重要）**：原版游戏中 `plrgfx\trnwarrior/trnrogue/trnsorcerer/trnmonk.trn`
     **均不存在**（MPQ 内 plrgfx 下仅 `infra.trn`/`stone.trn` 两个全局 TRN 真实存在），
     故按职业默认 TRN 路径解码时为**忠实的 no-op**（`trn_applied:false`，仍用调色板上色），
     与游戏内玩家着色行为一致。真正存在的 `plrgfx\infra.trn`（红外视觉红染）/ `plrgfx\stone.trn`
     （石化灰染）可经 `trn_name` 显式施加，作为可视的 TRN 效果演示。
   - **前端**：`web/` 新增「应用 TRN」开关 + TRN 预设下拉（职业默认 / infra 红外视觉 /
     stone 石化），默认关闭（原调色板模式）；开启即传 `apply_trn:true`，meta 行显示 `TRN: <name>`。

4. **（阶段 3）怪物 / NPC 目录合成**（已实现，2026-11-07）
   - 怪物（`monsters\`）与 NPC（`towners\`）精灵**没有 MPQ listfile**，无法像其它家族那样靠
     哈希恢复真实名。故 `sprite.list` 在 `family:"monsters"` / `"towners"` 时**改从 DevilutionX
     数据表合成目录**：
     - 怪物：`monstdat.tsv` 的 `assetsSuffix` + `frames[6]`（逗号分隔的
       Stand/Walk/Attack/GotHit/Death/Special 每方向帧数），配合固定 `Animletter="nwahds"`
       （`n/w/a/h/d/s`）→ 每条 `monsters\{assetsSuffix}{L}.cl2`，带 `anim` 友好名、
       `frames_per_dir` 与自动查表宽度（`monstdat.tsv` 的 `width` 列）。
     - NPC：`towners.tsv` 的 `animPath`（已是 `towners\...` 全路径）+ `animWidth` →
       `towners\{animPath}.cel`（单方向）；另硬编码 `towners\animals\cow.cel`（不在 TSV 内）。
   - 合成条目复用 `recovered.json` 精灵条目的字段形状（`name/family/class/type/width`，
     外加 `anim`/`frames_per_dir`），`archive_path` 留空——由前端怪物查看器的 **MPQ 下拉框**
     指定实际解码用的 MPQ（怪物/NPC 真实存在于 DIABDAT.MPQ 等）。
   - `sprite.decode` 对怪物额外返回 **`directions`** 字段 = `帧数 / monstdat 每方向帧数`
     （如僵尸 Stand：88 帧 ÷ 11 = 8 方向），前端据此自动切分方向，无需手填。
   - 已加入 `integration_tests.cpp`（`testSpriteListSynthesis` 断言合成含
     `monsters\zombie\zombien.cl2` 且 `frames_per_dir==11`；`testSpriteDecodeMonster`
     对真实 MPQ 断言 `directions==8` / `frame_count==88`）。
   - **跨 MPQ 解码**：Hellfire 怪物 CL2 位于 `hellfire.mpq` 而非 `DIABDAT.MPQ`，故前端
     `buildDecodeArgs` 把 `sprite.list` 收集到的**全部 MPQ** 作为 `sprite.decode` 的 `archives`
     搜索栈（并统一 `/` 路径避免同一档案两种拼写被重复打开），后端 `extractBufAny` 逐个档案
     查找精灵/调色板。回归测试 `testSpriteDecodeCrossArchive`：单 MPQ 解码 Hellfire 怪物应失败、
     传入全栈应成功。修复前全量 325 条合成目录 137 条失败，修复后 116 条转为成功。

5. **（特效叠加层 alpha 合成）**（已实现，2026-11-08）
   - 通用叠加子系统：任一浏览模式选中基础精灵后，画布下方「特效叠加层」面板（默认折叠）可把任意
     特效精灵解码为独立图层，按 `(dir, frame)` 对齐后 alpha 合成到底图当前帧上。
     - 快速加：角色模式列出 `plrgfx\{c}\{caw}\{caw}{lm/fm/qm}.cl2`（闪电/火/魔法）；
       怪物模式列出该怪物的 Special `s` 动画（把动画字母 `n/w/a/h/d` 换成 `s`）。
     - 通用加：下拉列出当前目录全部精灵，可任意选取做底图叠加。
     - 每层可启用/停用、设 `x/y` 偏移、移除；meta 行显示「· 叠加层 N」。
   - 前端：`web/src/viewer.js` 的 `drawComposite(canvas, 底图RGBA, w, h, 叠加数组)`（每叠加层建离屏
     Canvas 后 `drawImage` 源 alpha）；叠加层状态在 `web/src/main.js`（`overlayLayers`、`lastDecodeArgs`，
     `buildDecodeArgs` 复用基础精灵的调色板/TRN 构造解码参数）。切换基础精灵时清空叠加并刷新快速加按钮。
   - 覆盖用户要求的四类作用域：怪物查看器里的怪物、角色查看器里的技能、通用底图+叠加选取器、
     monster 的 Special 动画。

### 10.3 前端目录结构（JS ES Module + Canvas，零构建，无 Node 运行服务）

> 阶段 2 脚手架已落地（见 `web/`）。实现要点：**纯 JavaScript ES Module**，浏览器
> 原生加载，**无需 tsc/npm 构建**——环境无 TypeScript 工具链，故用 ES Module 替代
> 计划里的 `.ts`（模块划分、职责与计划一致）。运行只需一个静态文件服务器。

```
web/                              # 浏览器前端（不读 MPQ/文件，仅 WebSocket 调后端）
├── index.html                    # 入口：连接栏 + 目录树(<ul>) + 当前资源名 + 错误条 + Canvas + 控制条
├── styles.css                   # 极简暗色样式
├── src/
│   ├── client.js                 # McpClient：WS + JSON-RPC（id 配对，并发安全）
│   ├── catalog.js                # sprite.list 结果 → 嵌套树（按类型合并 / 按 MPQ 包）→ 可点击可折叠 DOM
│   ├── viewer.js                 # rgba_base64 → atob→ImageData→putImageData（最近邻放大）
│   └── main.js                   # 编排：connect → sprite.list(+kind:palette 填下拉框) → 点击 → sprite.decode → 绘制
└── README.md                     # 开发者说明 + 运行步骤 + 与后端契约
```

- 前端**不引入 Node 作为运行服务**：`web/` 是纯静态文件，用任意静态服务器
  （`python -m http.server 8000`，从 `web/` 目录起；或 `npx serve .`）托管，浏览器
  打开 `http://localhost:8000/`，连 `ws://localhost:18765`（后端 `mcp-server --ws 18765`）。
- `ES module`（`type="module"`）不能在 `file://` 下加载，必须经 http 托管；
  WebSocket 从 http 页连 `ws://localhost` 不受 mixed-content 限制。
- 绘制用 Canvas 2D `ImageData`（rgba_base64 即原始像素，最灵活）；如后续要 3D/滤镜
  再考虑 WebGL，但不在阶段 2 范围。
- 交互细节（skeleton 已实现）：① 主视窗顶部**醒目显示当前资源完整路径**（`currentName`）；
  ② 调色板改为**下拉框**，由 `sprite.list(kind:"palette")` 拉取档案内全部 `.pal` 填充，
  切换即对该精灵重新解码；③ 任何失败（暂不支持的格式 / 缺 archive_path / 解码报错）都通过
  **错误条（errorBanner）显式提示**，而非静默。
- 左侧**资源树**（`catalog.js`）由 `sprite.list` 结果构建，支持两种分组方式可切换：
  **按类型合并**（`family→class→type→精灵`）与 **按 MPQ 包**（先按所属 MPQ，再按档案内
  目录路径逐层展开）；两种模式下分支节点均**默认收起**，点击标题展开/收起，并提供
  「展开全部 / 收起全部」。切换分组方式无需重新请求后端（前端缓存上次 `sprite.list` 结果）。

**契约说明**：`sprite.list` 每条精灵除 `archive`（档案名，如 `DIABDAT.MPQ`）外，
还返回 `archive_path`（recovered.json 里该档案的 `path` 字段，即可直用于
`sprite.decode` 的 MPQ 路径）。前端 `main.js` 直接用 `archive_path` 解码，界面已无
Archive 输入框——点选精灵即可直接解码，无需用户手填路径（缺失时状态栏报错）。
顶层 `archives[]` 给出 recovered.json 的**全部** MPQ 路径（含不含任何 CL2/CEL 精灵
的 `devilutionx.mpq`），前端 `loadCatalog()` 以此构建 `allArchives` 并整栈传给
`ui.build` / `scene.build`；若只从 `sprites[].archive_path` 推导会漏掉这些档案，
导致 `.clx` 界面底图 `missing`、依赖 `ui_art\diablo.pal` 的 CLX `decode_error`。

### 10.4 工作顺序（建议）

1. 后端：`sprite.decode` 加 `width:"auto"`（复用 render_sprite 查表）→ ✅ 已完成（三套件测试通过）。
2. 后端：新增 `sprite.list`（读 recovered.json + 数据表）→ ✅ 已完成（三套件测试通过）。
3. 后端（可选）：`sprite.decode` 加 `apply_trn` 上色 → ✅ 已完成（见 §10.2.3，含前端开关）。
4. 前端：脚手架 `web/`（✅ 已落地，纯 JS ES Module，零构建）：`client.js` 连 WS →
   `sprite.list` 取目录 → 点击即 `sprite.decode` 取 RGBA → `viewer.js` 绘制；
   已支持"选精灵 → 看单帧"（方向/帧滑块翻帧）+ ▶播放。
5. **独立「角色查看器」面板 + 8 方向动画播放（✅ 已完成，2026-11-07）**：
   - 顶部「浏览模式」切换：**目录浏览**（原目录树）与 **角色查看器** 并存，共用同一
     Canvas / 调色板 / 方向 / 播放控件。
   - 角色面板：`sprite.list(family:"plrgfx")` 一次性构建索引（class → 真实存在的
     type / 盔甲 / 武器组合），按 `职业 → 动作 → 盔甲/武器` 选择器拼
     `plrgfx\{class}\{c}{a}{w}\{c}{a}{w}{type}.cl2` 并解码；仅暴露 MPQ 真实存在的组合。
   - 8 方向：玩家 CL2 为**方向优先**（8 方向 × N 帧），前端按 `总帧数 / 方向数` 切分，
     提供方向滑块 + 帧滑块 + ▶播放/⏸ + fps 速度；方向数可选 1/2/4/8（plrgfx 默认 8）。
   - 装备**不做 alpha 合成**（D1 把武器/护甲 bake 进文件名前缀，组合浏览=枚举文件名）；
     角色施法动作（`lm`/`fm`/`qm`，**不是**火球类攻击效果）作为可浏览动画纳入动作下拉，
     并可通过「特效叠加层（alpha 合成）」面板（见 §10.2 第 5 项）按图层叠加到底图上。
   - 改动 `web/index.html` + `web/src/main.js` + `web/styles.css`，**后端无改动**。
6. 查看器能力复用（阶段 3 目标，已实现）：
   - 怪物/NPC 查看器复用同一 `sprite.list` + `sprite.decode` 链路，仅家族不同；
     新增「怪物 / NPC 查看器」浏览模式（`web/index.html` 第三单选 + `monsterPane` 面板）：
     选 MPQ（来自目录归档的 `archive_path`）→ 选怪物 → 选动作（Stand/Walk/Attack/…）即解码播放。
   - 怪物方向数由 `sprite.decode` 返回的 `directions` 字段**自动**切分（不再手填）；
     NPC 为单方向 `.cel`。两者自动查表宽度（`monstdat.tsv` / `towners.tsv`）。
   - TRN 上色（见 §10.2.3，已实现）：`sprite.decode` 加 `apply_trn` 接入职业 TRN 与
     每精灵 TRN；前端「应用 TRN」开关可切换原调色板模式 / TRN 模式，meta 行显示应用状态。
7. **技能 / 战斗查看器的方向模型（✅ 已完成，2026-11-08）**：`web/src/main.js` 把
   `sprite.decode` 的扁平帧按方向拆成 `FrameSet = {groups:[frames[]...], directions}`：
   `makeFrameSet` 按 `总帧数 / directions` 分组，`frameSetAt(fs,fi,dir)` 只从
   `combatGroupIndex(directions,dir)` 选中的那一组取帧（帧号对组长度取模），
   `frameSetBudget` 取各组最大帧数作为**方向无关**的时间线预算。因此播放循环
   （`startCombatPlay`）只推进帧号，从不改变方向；旋转方向也无需重建 `combatTimeline`。
   - `combatLayout()` 以施放者固定的场景中心为原点，按 8 向向量放置目标；
     施放者朝向 = d、目标朝向 = (d+4)%8（回身面向施放者），两者位置与朝向在
     每次 `dir` 滑块变更时重算。施放者由 `cast_action.anchor`（`spell.list`）判定，
     所以「怪物远程攻击」来源下施放者/目标互换、布局随之变化。
   - `viewer.js::drawScene` 的 actor `x/y` 是绝对定位；`pushCombatActor` 用
     `中心 - 帧尺寸/2` 换算左上角，并按 `y` 给 `z` 做深度排序。投射物沿
     施放者→目标插值，命中/debuff 落在目标上。
   - **关键事实**：`missile_sprites.tsv` 的 `numFrames>1` 编号文件是**方向**
     （`Source/tables/misdat.cpp::LoadGFX` 的 `LoadMultipleCl2Sheet<16>` +
     `spritesForDirection(Direction16)`），不是连续动画帧；`decodeLayerFrames`
     把每个文件保留为一个方向组，不再平铺成跨方向的 224 帧动画。
     玩家 CL2 由 `combatDirsOf` 按 `plrgfx` 前缀固定 8 方向，怪物用后端
     `directions` 字段，导弹为 1 方向。
   - **面板布局（✅ 已完成，2026-11-08）**：左侧目录树（`#catalogPane`）**仅在「目录浏览」**
     模式显示，其余查看器（角色 / 怪物 / 战斗）隐藏该树；战斗查看器为三栏——
     左 `combatPane`（来源/技能/职业/阶段控制）、中 `stage`（Canvas 显示窗口）、
     右 `combatDetailPane`（英文名 / 中文名 / 描述 / 各级法力 / 各级视觉参数表），
     法术详情竖直滚动置于显示窗口右侧。
   - **显示放大倍数（✅ 已完成，2026-11-08）**：控制条新增「放大倍数」滑块（1–8，默认 2）。
     **Canvas 固定为 1280×720（720p）逻辑视口**（`viewer.js` 的 `VIEW_W/VIEW_H`），缩放
     **在绘制时施加**：每帧 bitmap 解码到 native 尺寸的离屏 canvas，再用
     `ctx.drawImage(img, dx, dy, nativeW×zoom, nativeH×zoom)` 画到固定视口中央——这正是
     Canvas 内建的绘制缩放模式；配 `imageSmoothingEnabled=false` 为最近邻清晰放大。
     战斗场景的 actor 位置与尺寸同为 native 坐标，故 sprite 间距随同一倍数放大。
     改变倍数只重绘、**不改变 canvas 元素/后备存储尺寸**；拖动滑块即重绘
     （`renderCombatStage` / `redrawCurrent`）。
     静态页面对 CSS/JS/模块导入加了版本查询串（`?v=`）以强制浏览器越过旧缓存。

> 该顺序让"列清单 → 渲染 → 显示"尽快端到端跑通，且后端每一步都可独立单测，
> 符合项目"先理解、再使用；CLI / 可验证产出优先"的章程。

### 10.5 场景查看器（第 5 种浏览模式，2026-11-09）

新增 `web/index.html` 第 5 个 radio `value="scene"` + `scenePane` 面板 + `.scene-view`
（替代共享 Canvas）。`web/src/main.js` 增加 `isSceneMode()` 守卫（精灵管线的
`showFrame`/`redrawCurrent`/`startPlay`/缩放/调色板/TRN/帧/方向处理器在场景模式早返回，
互不干扰）、`scene.list`/`scene.build` 调用、缩放平移与叠加层逻辑。

- **数据路径**：`scene.build` 返回 `image_base64` / `overlay_base64`（PNG 字节 base64），
  前端把 base64 解码为 Blob → `createImageBitmap` → `drawImage` 到两张绝对定位
  `<canvas>`（`sceneBase` / `sceneOverlay`）；**不经过** `viewer.js` 的 RGBA 精灵帧路径。
  不创建 `<img src>`，也不拼巨大的 `data:` URL。原生大图只落盘，WS 只传 ≤1000px 预览。
  PNG 的 IDAT 由 `src/decoders/deflate.cpp::compressZlib`（fixed-Huffman + 贪心 LZ77）
  生成，`scene.build(tristram)` 预览约 343 KB base64，远低于 Firefox `data:` URL ~2 MB
  上限；早期 stored 块版本 ≈2.2 MB 会触发「无法显示的图片/空图」。
- **对齐**：两张图由同一 `SceneProjection` + 同一整数 `scale` 生成，尺寸完全一致；
  `applySceneTransform()` 对共享容器 `#sceneLayers` 施加一组 `width/height/left/top`，
  两张 `<canvas>` 用同一 transform，缩放 1–8×（滑块/滚轮）、视口内拖拽平移、
  「复位视图」全图适配、`image-rendering:pixelated` 保证清晰放大。
- **叠加层**：半透明主网格线（每 8 格）+ 每格 SOL 着色（红=Solid / 蓝=透明 /
  黄=BlockLight），开关控制显隐；「提亮」给夜景城镇加 CSS
  `brightness(1.7) contrast(0.85) saturate(1.1)`（`web/styles.css` 的 `.scene-layers.bright canvas`），
  开关**默认勾选**且 `loadScene()` 每次按开关状态施加 `.bright` 类，避免 Tristram 夜景过暗
  （均值亮度 13、67% 像素 <24）被误认为空图。
- **光照**：新增「光照（dLight 暗部 + 中心火把）」开关，`loadScene()` 传
  `scene.build(lighting:true)`；后端按场景 ambient + 中心 radius-15 火把生成每格 dLight，
  用 `LightTables` 烘焙调色板，城镇全亮、地牢/地狱呈现暗部与光照范围。切换开关会重建场景。
- **MPQ 路径**：默认用 `sprite.list` 收集到的 `allArchives` 栈（后端 `Archive` 统一
  `/`→`\` 归一化），可在 `sceneArchive` 输入框手动覆盖主档案。
- 静态资源 `?v=20261109b`（CSS / main.js / 3 个 import）一并刷新缓存。

> 场景模型、资产路径、两遍绘制与「黑块」根因见 `docs/PROJECT.md`「游戏场景如何组织」
> 与本文 §9.2/§9.3；前端使用说明见 `web/README.md`。

---

## 11. 阶段 5：GUI 布局还原（目标 4）

### 11.1 分层（两个 header-only 纯模块 + 一个工具编排层）

仿 `spell_tables.h` 的做法，把纯数据与纯像素逻辑做成 header-only，令
`tests/unit_tests.cpp` 直接 `#include` 即可全量单测（**unit 的 g++ 命令不需要加任何 .cpp**）：

- `src/tools/ui_layout.h`（namespace `ui_layout`）：`Rect/ArtRef/Element/Panel/Screen`
  结构 + `screens()` 静态目录（18 屏）+ `findScreen/validateScreen/uniqueArtRefs/
  needsWidthList/overlayBoxes/typeName/overlayColor`。零外部依赖（仅标准库）。
- `src/tools/ui_render.h`（namespace `ui_render`）：`Canvas/fillAll/putPixel(src-over)/
  blit/drawRect/drawLabel(内置 5×7 编号字模)/rasterizeOverlay`。零外部依赖。
- `src/tools/ui_tools.cpp`：唯一触碰 engine/decoders/json 的编排层，`registerUiTools`
  注册到 `src/main.cpp`，被 GLOB 纳入；只编进 integration 测试命令。

### 11.2 坐标与 palette 分工（关键实测结论）

- 所有 `rect` 是 **640×480 绝对逻辑坐标**（`display.cpp` `UISize{640,480}`）；DevilutionX 里
  相对分区原点的值另存 `panel` + `panel_rect`。分区：`MainPanel{0,352,640,128}`、
  `LeftPanel{0,0,320,352}`、`RightPanel{320,0,320,352}`。
- **两套调色板并存**（真实 MPQ 验证）：`ui_art\diablo.pal`（devilutionx.mpq）低索引是纯
  原色，是**菜单 / CLX 转换调色板**——正确用于 `dvl_lrpopup.clx`/`dvl_but_sml.clx` 等
  DevilutionX 转换资源，但套原版游戏内 `.cel` 会出**彩虹噪点**。游戏内 HUD/面板 CEL
  （`ctrlpan\panel8`, `p8bulbs`, `data\*.cel`）与游戏内面板 CLX（`charbg`/`stash`）必须用
  **关卡调色板** `levels\towndata\town.pal`（`LoadRndLvlPal(DTYPE_TOWN)`），实测 0% 噪点。
  菜单 `.pcx` 用**内嵌调色板**。
- 因此 `ArtRef.palette` 是**逐资源权威字段**；`ui.build` 维护一个 **palette bank**（按名缓存
  768B RGB 表），`decodeUiArt` 按元素各自的 `ArtRef.palette` 取用。一屏可同时用
  `town.pal`（游戏内 CEL）与 `diablo.pal`（对话框 CLX），互不串色。`palettes[]` 返回每项状态。
- CEL/CL2 帧宽不存于文件，取布局目录里写死的 `width`（这些 UI 资源不在 `sprites.tsv`，
  **不用 `width:"auto"`**）；未恢复的宽度进 `needs_width`，`ui.build` 跳过该元素绘制并标
  `decode_status:"width_unknown"`，保留框线、不伪造。

### 11.3 `ui.build` 渲染管线

`findScreen` → `archiveStack`（`archive` + `archives` 全栈）→ 预填不透明背景的 640×480
RGBA 画布 → 按名加载 palette bank → 元素按 `z`（同 z 按 index）升序：PCX `decodePcx`
（内嵌调色板 / `transparent_index`）、CEL/CL2 `celToClx`/`cl2ToClx`（布局 `width`）再
`decodeClx`、CLX 直接 `decodeClx`（768B RGB）→ `blit`（src-over、裁剪、透明跳过）→
`encodePNG` 出底图 + `rasterizeOverlay`（按 type 分色框线 + 编号角标）出对齐叠加层 →
两张 PNG 落盘 + `base64::encode` 返回 + sidecar JSON。抽不到/解码失败/宽度未知的资源按
`skip_missing_art` 跳过，工具整体仍成功。

### 11.4 文字策略与前端第 6 模式

后端 PNG **不烧录文字**（游戏位图字体 `fonts\{size}-{row}.clx` 在 MPQ 仅恢复基本 ASCII
行、中文缺字）：`ui.layout`/`ui.build.elements` 的 `text/font_size/color/align/flags` 是
前端绘制的唯一数据源。前端 `web/` 加第 6 个 radio/pane（复用 scene 的 PNG→canvas
`sceneDrawPng` 与 CSS `width/height` 缩放范式），三层 canvas 叠加：`#guiBase`（真实美术）
+ `#guiText`（`drawGuiText` 系统字体近似、占位符替换、可开关）+ `#guiOverlay`（后端框线
PNG）。左栏「元素」下拉框（`populateGuiElementSelect`）列出当前界面全部元素，选中者由
`selectGuiElement` 把详情填进左栏 `#guiInfo` 并在 `#guiSelectBox` 高亮其矩形——**不用鼠标
悬停**。`isViewOwnedMode()`（scene 或 gui）让共享精灵控件在这两种模式下成为 no-op。
`?v=20261110a` 统一刷新缓存。

> **界面动画（明确不实现，2026-11-10 项目取舍）**：`ui.build` 每次调用只合成**一张静态
> 640×480 PNG**，前端也只有单张 `#guiBase`；界面动画元素（标题画面 Logo 火焰等）不播放
> 序列帧、不提供「播放动画」按钮。要播放就得改上述分层或把合成移到其它层，属于方案级
> 调整，本项目不为此改动，也不新增工具。详见 `docs/UI_LAYOUT.md` §6.1。

> **加载状态一致性（2026-11-10）**：`loadGuiScreen` 用单调递增的 `guiLoadSeq` 作请求代号，
> **每次调用（含被在途守卫丢弃的那次）都自增**，`stale() = seq !== guiLoadSeq` 判定是否已有更新
> 请求；在途期间的任何请求（跨屏切换 **或同屏重载**，如编号角标开关）都由 `finally` 以最新参数
> 补发，不会静默丢弃，且补发自带新代号故收敛终止。提前 `return` 前用 `resetGuiElementView()`
> 只复位视图（保留选中簿记供同屏补发恢复）；`clearGuiElementState()` 在初始化 / 加载失败 / 无档案
> 时复位下拉框、详情、高亮框并清空三张画布，失败后允许重试。同一屏重载用 `preserveIndex` 保留
> 选中项。`res.elements` 经 `Array.isArray` 校验，渲染时对 `null` / 缺字段元素做防御，编号仍与
> 后端叠加层角标对齐。

> 18 屏逐界面转录表、资源清单、调色板实测数据与延后项见 `docs/UI_LAYOUT.md`。
