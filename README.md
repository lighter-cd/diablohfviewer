# DiabloHFConverter

Diablo 1 / Hellfire 的 MPQ 研究与精灵查看工具。**后端为纯 C/C++ 原生 `mcp-server`**，
不依赖 Node.js 运行时；通过两种传输对外提供工具：

- **stdio**：给 AI Agent（Claude Desktop / CLI 工具）用的标准 JSON-RPC 2.0。
- **WebSocket 桥**：给浏览器前端（`web/`，JS ES Module + Canvas）用，复用同一套 tool handler。

前端只负责把服务端解码好的 RGBA 帧画出来；所有 MPQ 读取与解码都在本地 C/C++ 服务端完成。

## 目录结构

| 目录 | 说明 |
|------|------|
| `src/engine/` | StormLib 封装（open / list / extract / verify） |
| `src/decoders/` | 格式解码器（palette / clx / cel / cl2 / pcx / png / `level_*` 关卡、场景、光照与寻路管线） |
| `src/tools/` | Tool Registry + `mpq_tools` + `sprite_tools` + `map_tools`（`map.build`）+ `scene_tools`（`scene.list`/`scene.build`/`scene.path`）+ `level_tools`（`level.inspect`/`level.generate`）+ **`ui_tools`（`ui.list`/`ui.layout`/`ui.build` 界面布局还原）** |
| `src/mcp/` | MCP 服务端（jsonrpc / stdio / ws 桥） |
| `3rdparty/StormLib/` | StormLib 源码（git submodule；构建脚本见仓库根 `build-stormlib.sh`） |
| `mpq-info/` | MPQ 文件名恢复字典与产出（参考数据，非代码） |
| `web/` | 浏览器前端查看器（`src/*.js` ES Module + Canvas，零构建，连 WS 桥） |
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

### Web 精灵查看器（阶段 2）

后端起 WS 桥后，用静态服务器托管 `web/` 目录即可在浏览器查看精灵（前端不读 MPQ）：

```bash
build-mcp/mcp-server.exe --ws 18765          # 1) 后端 WS 桥（端口 18765）
cd web && python -m http.server 8000        # 2) 静态托管（ES Module 需经 http，不能用 file://）
#    浏览器打开 http://localhost:8000/ ，点 Connect → 选精灵 → 看帧
```

查看器结构见 `web/README.md`；与后端契约、已知限制见 `docs/ARCHITECTURE.md` §10.3。

### 技能 / 战斗查看器（第 4 种浏览模式）

以**施放者为中心**同屏显示 施放者 + 目标 + 攻击/施法效果。选「来源」（玩家技能 / 怪物远程攻击）与具体技能后：

- 拖动「方向」滑块：目标被放到施放者朝向的方位并**回身面向施放者**，两者的位置与朝向同步重算；场景固定 8 个方向；
- 按「▶ 播放」自动走完 **施法 → 投射 → 命中 → debuff** 全阶段时间线（`自动连播全阶段` 可关闭，改为只循环当前阶段）；**每个阶段都在当前方向内播完，播放不切换方向**（编号 `missiles\{name}{N}.cl2` 是各方向的动画，只取当前方向那一份）；
- 「◀ / ▶」阶段按钮单独查看每个阶段；
- 选「怪物远程攻击」时施放者与目标角色互换（怪物施放、玩家成为目标），场景位置与朝向随之变化；
- debuff 用后端 `apply_trn`（`stone.trn` 石化 / `infra.trn` 红外等）对**目标**层做**近似**着色，或叠加 `manashld` / `shatter1` 等状态精灵。

数据来自 `spell.list`（编号特效文件带显式 `width`，由前端直接传给 `sprite.decode`）；`recovered.json` 只恢复了单帧 missiles 名，火球 / 火墙等核心编号特效不在其中，故必须经此工具发现。

## 工具一览（使用者角度）

| 工具 | 说明 |
|------|------|
| `mpq.list` | 列出归档内文件（无 listfile 时给 `FileNNNNNNN.xxx` 占位名） |
| `mpq.extract` | 抽取单文件（返回 base64 或落盘路径） |
| `mpq.verify` | 校验某文件是否存在 |
| `mpq.recover` | 用字典 `dict_path` 恢复真实文件名 |
| `mpq.dump` | 前 N 字节十六进制预览 |
| `palette.load` | 加载 `.pal` 调色板为 RGBA（`src/decoders/palette.cpp`） |
| `sprite.decode` | 解码 `.clx` / `.cel` / `.cl2` / `.pcx` 精灵为 RGBA 帧；`.cl2`/`.cel` 传 `width` 整数或 `"auto"` 自动按路径查表。可选 `archives`（额外 MPQ 路径数组）：精灵/调色板可来自与 `archive` 不同的 MPQ，用于跨档案解析调色板。**损坏/截断的 CL2/CEL 或错误 `width` 会返回错误而非崩溃**（`src/tools/sprite_tools.cpp`、`src/decoders/cl2.cpp`、`src/decoders/cel.cpp`） |
| `sprite.list` | 枚举 `recovered.json` 中真实存在的精灵名（按 family/class/type 分类并带自动帧宽），供前端建目录树（`src/tools/sprite_tools.cpp`） |
| `spell.list` | 从 DevilutionX 数据表（`spelldat` / `misdat` / `missile_sprites` / `monstdat`）合成**玩家技能与怪物远程攻击的全阶段视觉目录**：每个条目按 施法 → 投射 → 命中 → debuff 四阶段组织；`sprite` 层给出编号 `missiles\{name}{N}.cl2` 文件清单 + 显式 `width`（编号文件**不能**用 `width:"auto"`），`trn` 层给出目标 debuff 的调色板重映射；另附 `impact_catalog` 候选命中特效。每条还带 `name_zh`（中文名，取自 `Translations/zh_TW.po`→`zh_CN.po` 的 `msgctxt "spell"`，无翻译则回退英文名）、`description`（按已解析阶段链 + 玩家法力/智力消耗合成的中文说明），以及**法术级别参数**：`level`（`spelldat.bookLevel`，法术等级/阶）、`staff_level`、`mana_cost`、`mana_multiplier`（每升 1 级训练减少的法力）、`min_mana`、`staff_min/staff_max`，与 `mana_by_level`（按 `GetManaAmount` 公式算出的 1..bookLevel 各级基础法力 `{level, mana}`，含 `min_mana` 下限，未乘职业系数）；另含 `visual_by_level`（玩家 1..bookLevel / 怪物 1..16 各级**视觉参数**，逐法术从 DevilutionX handler 转录：持续时间 `duration_ticks`/`duration_sec`、弹速 `speed`、范围 `range_tiles`/`radius_tiles`、弹射物数 `count` 及法术特有字段如 `segments`/`wall_length_tiles`，含角色/地下城层依赖的项标 `_note`；无级别视觉缩放的法术为空）。**怪物攻击侧**除 `GetMissileType()` 的 14 类远程 AI 外，还覆盖**在 AI 函数里直接 `AddMissile` 的形态**（Counselor 按智力发 Firebolt/ChargedBolt/LightningControl/Fireball + 近身双层 Flash、Familiar 闪电、Gloom 冲撞、Mega 近身地狱火），每个形态单独一条并有 `variant` / `condition` 字段（`id` 为 `AI` 或 `AI.variant`）；`unique_monstdat` 的唯一怪会额外叠加，故 Hork Demon（HorkSpawn）、Zhar / Lazarus（按智力 3 归入 Counselor.Fireball）、Red Vex / Black Jade（LazarusSuccubus）均按真实 AI 归属。另含 **`abilities[]` 引擎级怪物特殊能力**：`Blink` 受重击瞬移、`AcidPuddle` 酸怪死亡留池、`Hidden` 潜行系隐身，每条带 `monsters` / `trigger` / `source_ref`（Diablo 1 没有 D2 式 Thunderstorm / Waller / Vortex / Shielding 词缀）。另含 **`traps[]` 陷阱法术** 10 条：墙上机关陷阱（箭矢 / 火焰弹 / 闪电，按有效层数出现）、带陷阱宝箱（箭矢 / 火焰箭 / 新星，地狱火另有火焰之环 / 偷药水 / 吸法力）、地狱火喷火口与火焰拉杆，每条带 `objects`（宿主 `OBJ_*`）/ `trigger` / `source_ref`。当前为 51 技能 / 25 怪物攻击 / 3 特殊能力 / 10 陷阱。可用 `source` / `id` 过滤（`src/tools/sprite_tools.cpp`、`src/tools/spell_tables.h`、`src/tools/spell_visual_model.h`） |
| `map.build` | 解码 Diablo 1 / Hellfire 关卡（TIL / MIN / DUN / SOL / level CEL）并渲染完整等距地图 PNG + ≤400px 预览 PNG + 每格碰撞/光照/透明标志 sidecar JSON 到 `out/maps/`，返回摘要与 base64 预览。必填 `archive` + `cel`/`til`/`min`/`sol`，以及 `dun`（档案内名）**或 `dun_path`（本地 `.dun`，用于 DevilutionX 调试版导出的动态地牢）**；可选 `palette`（默认 `levels/l1data/l1_2.pal`；**Cathedral 在游戏里由 `LoadRndLvlPal` 随机选 `l1_1..l1_4.pal` 之一，`gendata/cut2.pal` 是过场调色板，不要用于地牢**）、`archives`（额外 MPQ 搜索栈）、`micro_len`（10 原版 / 12 地狱火）、`blocks`（10 / 16）、`out`。当前为 FullyLit 渲染（逐像素光照为运行时效果，延后）。L1 示例：`levels/l1data/l1.cel`/`l1.til`/`l1.min`/`l1.sol` + `levels/l1data/rnd2.dun`；动态地图见 `docs/MAP_GENERATION.md`（`src/tools/map_tools.cpp`、`src/decoders/level_*.cpp`） |
| `scene.list` | 列出可复原的**完整场景**（Tristram 城镇 + L1/L2/L3/L4 预设），返回 id/title/layout/网格尺寸/默认环境光，无需 MPQ（`src/tools/scene_tools.cpp`） |
| `scene.build` | 复原完整关卡场景：忠实 DevilutionX 两遍绘制（地板菱形 + 墙体）+ 不透明背景 + 空格预填，**0 透明像素、无黑块**。必填 `id` + `archive`，可选 `archives`（MPQ 搜索栈）、`out`（默认 `out/scenes`）、`preview_max_width`（默认 1000）、**`lighting`**（默认 false = FullyLit；true 时按 `ambient` + `lights` 应用 DevilutionX `LightTables` 暗部着色）、`ambient`（0..15，默认城镇 0 / 地牢 15）、`lights`（`{x,y,radius,offset_x,offset_y}` 微瓦片坐标数组）、`auto_light`（无显式光源时在场景中心加 radius-15 火把，默认 true）。返回 `image_base64`/`overlay_base64`（对齐的 SOL 叠加层）+ `preview_width/height` + `meta`（每格 `solid`/`transparent`/`block_light`/`block_missile`/**`walkable`**/`special`/`special_count` + 可选 `light` + `transparent_pixel_count`）；原生 PNG 落盘。场景：`tristram`、`l1_rnd2`、`l1_skngdo`、`l2_blind`、`l3_anvil`、`l4_diab1`（`src/tools/scene_tools.cpp`、`src/decoders/level_scene.cpp`） |
| `scene.path` | 在已解码场景的 SOL 碰撞网格上做 **8 方向 A\*** 寻路（忠实 `Source/engine/path.cpp`：直行 100 / 斜行 101，斜向不额外限制转角；目标是 Solid 时仍可到达，同引擎 `FindPath` 行为）。必填 `id` + `archive` + `from`/`to`（`{x,y}` 微瓦片坐标），可选 `max_steps`（默认 1024）、`preview_max_width`（与 `scene.build` 对齐）。返回 `found`/`cost`/`steps`/`walkable_count` + `path[]`（每格 `x/y`、原生 `screen_x/screen_y`、预览 `preview_x/preview_y`）（`src/decoders/level_path.cpp`、`src/tools/scene_tools.cpp`） |
| `level.inspect` | **关卡原始 tile 查看器**：给定 `archive` + `cel`/`til`/`min`/`sol`/`palette` + `blocks`/`micro_len` + `megatile`（1-based）或 `micros[]`，返回该 megatile 的 4 个 micro piece、每个 MIN block 的 `TileType`/`frame`/原始 `data`、所有引用到的 CEL 帧 PNG（base64）以及这些 micro 拼出的等距透明底预览 `assembly`。用于核对 `FillTile(60,70,71)` 这类地图对象（Tristram 水池 = megatile 71 → pieces 257–260 → town.cel frames 594–605）（`src/tools/level_tools.cpp`） |
| `level.generate` | **原生动态地牢生成（Cathedral L1–L4）**：按 `seed` 调 `DungeonSeeds[level]`，用 DevilutionX `drlg_l1.cpp` 的逐函数移植（同一 Borland LCG、同一 RNG 调用顺序）生成 40×40 megatile 地图；可选放置 Butcher / Skeleton King / Ogden's Sign 任务 set-piece（从 `archive`/`archives` 读取 `rnd6.dun`/`skngdo.dun`/`banner2.dun`）。必填 `archive` + `level` + `seed`，可选 `original_cathedral`（true=Diablo 原版，false=Hellfire）、`quests`（`vanilla`/`none`/`all`）、`out`（默认 `out/gen`）。写出 ExportDun 兼容的 `<level>-<seed>.dun`（可用 `map.build` 的 `dun_path` 渲染），返回 `tiles`（40 行 hex）、`floor_count`、`stairs_up_count`/`stairs_down_count`。已通过 DevilutionX 14 个 Cathedral 测试 fixture 的 0 差异校验（`src/decoders/dungeon_gen*.{h,cpp}`、`src/tools/level_tools.cpp`） |
| `ui.list` | 列出 **18 个可查看的 Diablo 1 / Hellfire 界面**（主菜单 / 标题 / 制作群 / 英雄·难度选择 / OK·YesNo / 设置 / 底部控制面板含血蓝球 / 角色·物品栏·法术书·任务日志 / 文本·网格商店 / 大·小对话框及其 CLX 变体）。**纯静态目录，无需 MPQ**。返回每屏 id/title/category/canvas/元素数/文字数/资源数/`needs_width`/源码位置；可选 `category` 过滤（`menu`/`hud`/`panel`/`store`/`dialog`）（`src/tools/ui_layout.h`、`src/tools/ui_tools.cpp`） |
| `ui.layout` | 返回单屏**完整结构化布局**（`ui.list` 的 `id`）：面板分区、每元素绝对 640×480 `rect`、类型、`label`、引用的美术 MPQ 名/格式/width/frame/palette、文字/字号/颜色/对齐/flags、`source_ref`。**文字不在后端烧录**：这些字段驱动前端近似字体文字层。无需 MPQ（`src/tools/ui_layout.h`、`src/tools/ui_tools.cpp`） |
| `ui.build` | 用**真实 MPQ 美术**按 DevilutionX 坐标离屏合成 640×480 界面 PNG（背景 PCX 内嵌调色板 / 面板 CEL / 血蓝球 / 按钮 CLX），**不透明背景、0 透明像素**，并生成对齐的元素框叠加层 PNG。必填 `id` + `archive`，可选 `archives`（**必传全栈**：`DIABDAT.MPQ` 存原版 `.pcx/.cel` + `town.pal`，`devilutionx.mpq` 存 `ui_art\diablo.pal` + `.clx`）、`out`（默认 `out/ui`）、`palette`（全局覆盖，一般不用）、`overlay`/`label_numbers`/`background`/`skip_missing_art`。返回 `image_base64`/`overlay_base64`（PNG）、每元素 `decode_status`（`ok`/`text`/`missing`/`width_unknown`/`decode_error`）+ `decoded_size`/`placed_rect`、每资源 `art[]` 状态、`palettes[]`（逐资源调色板，因一屏可混用 `town.pal` 与 `diablo.pal`）、`needs_width`、`art_ok/missing/error_count`，并写 `out/ui/<id>.png` + `<id>_overlay.png` + `<id>.json`（`src/tools/ui_layout.h`、`src/tools/ui_render.h`、`src/tools/ui_tools.cpp`）。详见 `docs/UI_LAYOUT.md` |

请求示例（JSON-RPC 2.0，stdio 与 WS 同构）：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"sprite.decode",
           "arguments":{"archive":"devilutionx.mpq","name":"data\\health.clx",
                        "palette_name":"ui_art\\diablo.pal"}}}
```

`ui.build`（务必传全 MPQ 栈，UI 美术跨档案）：

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call",
 "params":{"name":"ui.build",
           "arguments":{"id":"control_panel",
                        "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
                        "archives":["D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
                                    "D:/work/xt/diablo/DevilutionX-Hell Fire/devilutionx.mpq"]}}}
```

## 界面布局查看器（阶段 5 · 前端第 6 种浏览模式）

后端 `ui.build` 用真实 MPQ 美术合成界面底图 + 元素框叠加层；前端在 `index.html` 顶部
「浏览模式」切到 **界面布局查看器** 即可查看。连接步骤见上文「运行」。

- 左侧「界面」下拉由 `ui.list` 填充（18 屏），选界面或点「加载界面」触发 `ui.build`。
- 底图是**真实游戏美术**（主菜单背景、控制面板与血蓝球、角色/物品栏面板、商店、对话框等）。
- **文字**由前端按 `ui.layout`/`ui.build` 的 `text`/`font_size`/`color`/`align` 字段用系统
  字体**近似绘制**（游戏位图字体在 MPQ 中仅恢复基本 ASCII、中文缺字，故逐字渲染延后）；
  「显示文字」开关可一键关闭以核对纯美术底图，占位符 `{PLAYER_NAME}` 等替换为示意值。
- 「显示元素框叠加层」「显示编号角标」「缩放 1×/2×」可调；鼠标**悬停**画面元素即在左栏
  `#guiInfo` 显示该元素的类型/矩形/引用资源/解码状态/DevilutionX 源码位置。
- UI 美术跨 MPQ：`DIABDAT.MPQ` 存原版 `.pcx/.cel` 与 `levels\towndata\town.pal`，
  `devilutionx.mpq` 存 `ui_art\diablo.pal` 与 `.clx`；前端把 `sprite.list` 收集到的
  `allArchives` 全栈传给 `ui.build`。逐资源调色板分工与全部 18 屏转录表见
  `docs/UI_LAYOUT.md`。

## 测试

后端各单元均配合单元测试，分三个套件，全部在 `out/` 下，用 MinGW-w64 的 `g++` 直接构建运行。
（注意：`GLOB_RECURSE` 用于 CMake 的 `mcp-server`；新增 `src/**/*.cpp` 后需重跑一次
`cmake -S . -B build-mcp` 才会被纳入构建。测试用 g++ 单文件直接编译，把新增源文件加进命令即可。）

### 1. 解码器 + base64（无需 MPQ / StormLib）

```bash
g++ -I src -std=c++17 out/unit_tests.cpp \
    src/decoders/clx.cpp src/decoders/cel.cpp src/decoders/cl2.cpp \
    src/decoders/pcx.cpp src/decoders/palette.cpp src/decoders/png.cpp \
    src/decoders/deflate.cpp \
    src/decoders/sprite_width.cpp \
    src/decoders/level_decode.cpp src/decoders/level_cel.cpp src/decoders/level_render.cpp \
    src/decoders/level_scene.cpp src/decoders/level_light.cpp src/decoders/level_path.cpp \
    src/decoders/dungeon_gen.cpp src/decoders/dungeon_gen_l1.cpp \
    -static-libgcc -static-libstdc++ \
    -o out/unit_tests.exe && out/unit_tests.exe
```
覆盖：CLX 解码、CEL→CLX / CL2→CLX 往返、**CL2/CEL 畸形输入与错误 `width` 拒绝（不崩溃）**、**`spell_tables.h` 纯 join（TSV/CRLF、编号文件生成、AI→导弹、阶段派生）**、PCX 解码、调色板加载、base64 编解码往返、**`testDeflateRoundTrip`（`compressZlib`↔`decompressZlib` 回环 + 5 组代表数据 + length 3–258 × distance 全 30 码边界 15 万+ 组合，覆盖 RFC 1951 额外位）**、**PNG write round-trip（IDAT 用 `decompressZlib` 解压校验）**；以及**关卡/场景解码器**（`loadTil` / `loadMin` / `loadDun` / `loadSol` 解析、`buildMicroGrid` 0-based 微索引/kNoMicro 拼装、Square/Triangle 的 level CEL 重编码与 RGBA 帧解码、强制三角形解码、调色板 4 字节步长回归、`renderMap` 两遍绘制 + 不透明背景 + 0 透明像素 + 空格预填 + frame()==0 守卫、**dLight 光照表（Normal/Hell/Soft、falloff/cone、`buildLightMap` 环境光/点光源/越界光源）与 renderMap 逐格光照着色**、**8 方向 A* 寻路（开放网格/墙体/斜角/Solid 目标/maxSteps/边界）**、`buildTownScene` 4 sector/预填、`buildDungeonScene` Dirt 预填、`sceneProjection`、`encodePNG`）；以及 **`testDungeonGen` 动态地牢生成器**（LCG 逐步算术、`GenerateRnd`/state、DUN tile 层解析、按层任务默认值、同 seed 确定性/不同 seed 差异、楼梯/地板结构、缺失 set-piece 报错，并对 DevilutionX 的 14 个 Cathedral fixture（Diablo + Hellfire）做 40×40 全格 0 差异 oracle 校验；fixture 目录可用 `DHFC_DRLG_FIXTURES` 指定，缺失时 SKIP）。
源码路径：`src/decoders/*.cpp`、`src/tools/base64.h`、`src/tools/spell_tables.h`；测试脚本 `out/unit_tests.cpp`。

### 2. Registry + JSON-RPC（纯逻辑，无需 MPQ）

```bash
g++ -I 3rdparty -I src -std=c++17 out/core_tests.cpp \
    src/tools/registry.cpp src/mcp/jsonrpc.cpp \
    -o out/core_tests.exe && out/core_tests.exe
```
覆盖：`ToolRegistry` 注册/重名/分发/异常捕获/`list`，以及 `handleRequest` 的
`tools/list`、`tools/call` 与各错误码（-32600 / -32601 / -32602 / -32000）。
源码路径：`src/tools/registry.cpp`、`src/mcp/jsonrpc.cpp`；测试脚本 `out/core_tests.cpp`。

### 3. Engine + mpq_tools（需要真实 MPQ）

MPQ 路径取环境变量 `DHFC_TEST_MPQ`，缺省为本地 DevilutionX 安装的
`/d/work/xt/diablo/DevilutionX-Hell Fire/devilutionx.mpq`。若找不到该 fixture，
本套件打印 `SKIP` 并以退出码 0 结束（无游戏资源的环境也能跑）。

```bash
g++ -I 3rdparty/StormLib/src -I 3rdparty -I src -std=c++17 \
    out/integration_tests.cpp \
    src/engine/archive.cpp src/tools/registry.cpp \
    src/tools/mpq_tools.cpp src/tools/sprite_tools.cpp src/tools/scene_tools.cpp \
    src/tools/level_tools.cpp src/tools/ui_tools.cpp \
    src/decoders/palette.cpp src/decoders/clx.cpp src/decoders/cel.cpp \
    src/decoders/cl2.cpp src/decoders/pcx.cpp src/decoders/sprite_width.cpp \
    src/decoders/png.cpp src/decoders/deflate.cpp \
    src/decoders/level_decode.cpp src/decoders/level_cel.cpp src/decoders/level_render.cpp \
    src/decoders/level_scene.cpp src/decoders/level_light.cpp src/decoders/level_path.cpp \
    src/decoders/dungeon_gen.cpp src/decoders/dungeon_gen_l1.cpp \
    src/mcp/jsonrpc.cpp \
    3rdparty/StormLib/build/libStormLib.a \
    -lws2_32 -lwininet -static-libgcc -static-libstdc++ \
    -o out/integration_tests.exe && out/integration_tests.exe
```
覆盖：`engine::Archive` 的 `normalizePath`/`open`/`list`/`verify`/`querySize`/
`extract`/`extractToFile`，以及 `mpq.list`/`mpq.verify`/`mpq.extract`/`mpq.dump`/
`mpq.recover`（含用内置字典 `mpq-info/dict/diablo1-harvest.txt` 的端到端恢复）；
`sprite.list` 目录合成（过滤无 MPQ 文件的幽灵条目、cow 带真实宽度）、`sprite.decode`
跨 MPQ 搜索、**截断 CL2 返回干净错误而非崩溃**、`cow.cel` 的 `width:"auto"` 解码，以及
**`spell.list` 全阶段目录**（51 技能 / 25 怪物攻击 / 3 特殊能力 / 10 陷阱法术、Fireball 四阶段、Stone Curse TRN、
Fire Wall 双文件、Flash 双层、Torchant→Fireball、default→Arrow，以及 Counselor 四弹按智力分流 +
近身 Flash、Familiar 闪电、Gloom 冲撞、Mega 地狱火等 AI 内直接发弹的形态）；以及
**`testLevelMap` 关卡全链路**：从 `DIABDAT.MPQ` 抽真实 L1 资源（`levels/l1data/l1.{cel,til,min,sol}`
+ `rnd2.dun` + `levels/l1data/l1_2.pal`）跑通解析→重编码→等距合成并断言产出可见像素、**0 透明像素**（默认
`devilutionx.mpq` 无 L1 关卡，会自动探测同目录 `DIABDAT.MPQ`；两者皆无则 SKIP）；以及
**`testSceneTools` 场景全链路**：`scene.list` / `scene.build` / `scene.path` 从真实 MPQ 复原
Tristram 城镇，断言 96×96 网格、**0 透明像素**、base64 PNG 合法、叠加层/每格标志/`walkable`
数组长度；`scene.path` 返回起止格与原生/预览坐标；`scene.build(l1_rnd2, lighting:true)` 断言
环境光 15、中心火把点亮、`meta.light` 尺寸；`scene.build(l4_diab1)` 断言 44×44、microLen 12、
0 透明像素与 CEL 帧数。`testLevelInspect` 用 `level.inspect` 检查 Tristram megatile 71 →
micro 257–260 → frames 594–605，并验证组装 PNG。**`testLevelGenerate`** 用
`level.generate` 从 MPQ 原生生成 Cathedral L2（seed 1383137027）：断言 40×40、40 行 hex、
写出 54404 字节 ExportDun 兼容 `.dun`、`loadDun` 识别 40×40/1600 tile/transparency 层，
并验证同 seed 确定性；另测 L1 `quests:"none"`。
源码路径：`src/engine/archive.cpp`、`src/tools/mpq_tools.cpp`、`src/tools/sprite_tools.cpp`、
`src/tools/spell_tables.h`、`src/decoders/cl2.cpp`、`src/decoders/cel.cpp`、
`src/decoders/sprite_width.cpp`、`src/decoders/level_*.cpp`、`src/decoders/dungeon_gen*.cpp`；
测试脚本 `out/integration_tests.cpp`。

> 每轮改动后请完整运行以上三个套件，确保全绿。

### 4. 阶段 0 文件名恢复（研究/代码生成，需要真实 MPQ + DevilutionX 源码）

`mpq-info/recover_phase0.cpp` 读取 DevilutionX 权威数据表，按运行时拼接公式
（`plrgfx` / `monsters` / `missiles` 三类）批量生成候选文件名，与静态字典
`mpq-info/dict/diablo1-harvest.txt` 合并后逐档案校验，重建
`mpq-info/dict/recovered.json` 与 `recovered/*.txt`。

```bash
g++ -I 3rdparty -I 3rdparty/StormLib/src -I src -std=c++17 \
    mpq-info/recover_phase0.cpp src/engine/archive.cpp \
    3rdparty/StormLib/build/libStormLib.a \
    -lws2_32 -lwininet -static-libgcc -static-libstdc++ \
    -o out/recover_phase0.exe
out/recover_phase0.exe [devilutionx_root] [mpq_root]
```

- 默认 `devilutionx_root = D:/work/xt/diablo/DevilutionX`，
  `mpq_root = D:/work/xt/diablo/DevilutionX-Hell Fire`（均可用参数覆盖）。
- 源码路径：`mpq-info/recover_phase0.cpp`；产出：`mpq-info/dict/recovered.json`
  （汇总）与 `mpq-info/dict/recovered/<档案>.txt`（每行 `name<TAB>size`）。
- 当前恢复 **2,080** 个真实文件名（DIABDAT 1224 / hfmonk 291 / spawn 315 /
  hellfire 171 / devilutionx 63 / …），其中 TRN 调色板约 115 个。覆盖
  plrgfx/monsters/missiles/items/objects/towners 及 TRN 调色板等运行时拼接族。
  - 各家族的命名公式、DevilutionX 数据源与已验证扩展名见 `docs/RUNTIME_FILENAME_RULES.md`。

- **批量提取已验证文件**：`recover_phase0` 产出的 2,080 个名（见 `recovered.json` /
  `recovered/*.txt`）可一键抽取到本地，便于后续解码/查看。`mpq-info/batch_extract.cpp`
  读取 `recovered.json`（已含每个名对应的 MPQ 路径），逐档案 `extractToFile` 到
  `out/extracted/<档案>/<原名>`（原名含 `\` 子目录会自动建目录）。

  ```bash
  g++ -I 3rdparty -I 3rdparty/StormLib/src -I src -std=c++17 \
      mpq-info/batch_extract.cpp src/engine/archive.cpp \
      3rdparty/StormLib/build/libStormLib.a \
      -lws2_32 -lwininet -static-libgcc -static-libstdc++ \
      -o out/batch_extract.exe
  out/batch_extract.exe                  # 默认读 recovered.json，输出 out/extracted
  # out/batch_extract.exe [recovered.json] [out_root]   # 可覆盖路径
  ```

  - 源码路径：`mpq-info/batch_extract.cpp`；产物：`out/extracted/`（运行产物，不入库）。
  - 已验证可一次性抽取全部 2,080 个文件，0 失败。

### 5. 精灵渲染为 PNG（使用 src/decoders）

`mpq-info/render_sprite.cpp` 把 `out/extracted/` 里已抽取的精灵（CL2/CEL）经解码管线
渲染成 PNG：`CL2/CEL → CLX`（`cl2ToClx`/`celToClx`）→ `RGBA` 帧（`decodeClx`）→
`PNG`（`src/decoders/png.cpp` 调 `src/decoders/deflate.cpp` 的 **RFC 1951 fixed-Huffman +
贪心 LZ77** 压缩器，自带无外部 zlib 依赖，无需 `-lz`；输出可被标准 zlib 无损解压，
体积远小于早期的未压缩 stored 块版本）。

> 注意：CL2/CEL 文件**不存储帧宽**，需按 DevilutionX 动画表传入 `width`
> （如 warrior Stand=96、Attack/Death=128，详见各 `sprites.tsv` 的 `stand`/`attack`…列；
> 怪物/导弹/物品/物件/NPC 各有对应宽度）。把 `<width>` 写成 `auto` 即可让工具按
> 精灵路径自动查表取宽，无需手填；物品（CEL）无宽度表，仍需手填数字。调色板用任意
> 256 色 `.pal` 即可出形，选对游戏调色板则颜色正确。

```bash
g++ -I 3rdparty -I 3rdparty/StormLib/src -I src -std=c++17 -static \
    mpq-info/render_sprite.cpp src/decoders/sprite_width.cpp \
    src/decoders/clx.cpp src/decoders/cl2.cpp src/decoders/cel.cpp \
    src/decoders/pcx.cpp src/decoders/palette.cpp src/decoders/png.cpp \
    src/decoders/deflate.cpp \
    -o out/render_sprite.exe
# 用法: render_sprite <sprite> <palette.pal> <width|auto> [out.png] [frameIndex] [devilutionx_root]
# 自动查表（width 传 auto，默认 devilutionx_root = D:/work/xt/diablo/DevilutionX）
out/render_sprite.exe \
    out/extracted/DIABDAT/plrgfx/warrior/wlu/wluas.cl2 \
    out/extracted/devilutionx/ui_art/diablo.pal auto \
    out/render/warrior_stand.png
# 手动覆盖（数字宽度；如 Attack/Death 用 128）
out/render_sprite.exe \
    out/extracted/DIABDAT/plrgfx/warrior/wlu/wluat.cl2 \
    out/extracted/devilutionx/ui_art/diablo.pal 128 \
    out/render/warrior_attack.png 0
```

- 源码路径：`mpq-info/render_sprite.cpp` + `src/decoders/png.cpp` + `src/decoders/deflate.cpp`；
  PNG 编码器与 DEFLATE 压缩器含于单元测试（`writePNG` → 回读校验 round-trip、
  `testDeflateRoundTrip` 的 compress↔decompress 穷举回环）。
- 自动查表覆盖 5 类精灵：`plrgfx`（按 `sprites.tsv` 的 `stand`/`walk`/`attack`(含 `bow`)/`swHit`/`lightning`/`fire`/`magic`/`death`/`block` 列）、`monsters`（按 `monstdat.tsv` 的 `width`）、`missiles`（按 `missile_sprites.tsv` 的 `width`）、`objects`（按 `objdat.tsv` 的 `animWidth`）、`towners`（按 `towners.tsv` 的 `animWidth`）。`items` 无宽度表，需手填。
- 已验证：`plrgfx\warrior\wlu\wluas.cl2` 自动查表取宽 96 渲染为 96×96 战士立姿精灵（含盾牌），
  80 帧（8 方向 × 10）均可解码无错位；怪物 `monsters\acid\acida.cl2`（宽 128）、导弹
  `missiles\arrows.cl2`（宽 96）、物件 `objects\armstand.cel`（宽 96）、NPC `towners\drunk\twndrunk.cel`
  （宽 96）均经自动查表渲染成功。

### 6. 关卡地图与完整场景（阶段 4/5）

> **阶段 4（地图还原，目标 1）已完成（2026-11-09）**：解码/两遍渲染/完整场景/dLight/寻路/
> `level.inspect` tile 查看器/前端查看器与三套测试全部落地；逐像素 lightmap、地牢 DUN
> object layer 与 `dTransVal` 透明遮挡列为可选后续项。

> 2026-11-09 修正：渲染器改为忠实 DevilutionX **两遍绘制**（地板菱形 + 墙体）、
> **TIL 微索引 0-based**、**不透明背景 + 空格预填**，输出 **0 透明像素**，
> 彻底消除查看器把透明区压黑造成的「黑块 / 不完整」。详见 `docs/PROJECT.md`
> 「游戏场景如何组织」与 `docs/ARCHITECTURE.md` §9.2/§9.3。

**测试**：关卡/场景单测在 §1 的 `out/unit_tests.cpp`，真实全链路集成测试在 §3 的
`out/integration_tests.cpp::testLevelMap`（L1 `rnd2.dun`）与 `::testSceneTools`
（Tristram 城镇），均断言 **0 透明像素**。套件直接编入 `src/decoders/level_*.cpp` 与
`src/tools/scene_tools.cpp`；`mcp-server`（CMake `GLOB_RECURSE src/*.cpp`）新增源文件后
重跑一次 `cmake -S . -B build-mcp` 再 `cmake --build build-mcp --target mcp-server` 即可全绿。

**使用 `map.build`**（stdio 或 WS 的 `tools/call`，L1 cathedral 示例）：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"map.build",
           "arguments":{
             "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
             "cel":"levels/l1data/l1.cel",
             "til":"levels/l1data/l1.til",
             "min":"levels/l1data/l1.min",
             "sol":"levels/l1data/l1.sol",
             "dun":"levels/l1data/rnd2.dun",
             "palette":"levels/l1data/l1_2.pal"}}}
```

返回摘要（`image_path` / `preview_base64` / `meta_path` / `width` / `height` /
`grid_width` / `grid_height` / `frame_count` / `micro_count` / `solid_count` /
`transparent_count`），并写出到 `out/maps/`：

- `<dun>.png` —— 完整等距地图 RGBA PNG；
- `<dun>_preview.png` —— ≤400px 最近邻缩略预览；
- `<dun>.json` —— sidecar：`grid_width`/`grid_height`/`micro_len`/`frame_count` 与
  `solid` / `transparent` / `block_light` / `block_missile` 四个每格标志数组（来自 SOL 与 DUN 微尺度透明层）。

- 地狱火层级把 `micro_len`、`blocks` 改为 `12`、`16`，并指向对应 `nlevels/` 的 cel/til/min/sol。

**动态地牢（非固定地图）**：L1–L4、Hellfire L5/L6 都是按种子运行时生成的，MPQ 里没有整层
`.dun`。完整规则、种子/LCG 公式、各生成器流程、以及用 DevilutionX 调试版
`dev.level.exportDun()` 导出整层 `.dun` 的步骤，见 **`docs/MAP_GENERATION.md`**。

**直接生成 Cathedral（L1–L4）**：`level.generate` 已原生移植 `drlg_l1.cpp`，
给定 seed 即可得到与 DevilutionX 逐格一致的地图（14 个官方 fixture 0 差异）：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"level.generate",
           "arguments":{
             "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
             "level":2, "seed":271828182,
             "original_cathedral":true, "quests":"vanilla",
             "out":"out/gen"}}}
```

- `quests`：`vanilla`（按层默认任务）/ `none` / `all`；任务 set-piece 会自动从 MPQ 读取。
- 返回 `dun_path` 与 40×40 hex `tiles`。
- Cathedral L1–L4 **共用** `levels/l1data/l1.{cel,til,min,sol}` + `levels/l1data/l1_1..l1_4.pal`（默认示例用 `l1_2.pal`），
  `micro_len=10`、`blocks=10`（`l2/l3/l4data` 是 Catacombs/Caves/Hell，别混用）。

导出/生成后都可直接用 `map.build` 的本地文件参数渲染：

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call",
 "params":{"name":"map.build",
           "arguments":{
             "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
             "cel":"levels/l1data/l1.cel",
             "til":"levels/l1data/l1.til",
             "min":"levels/l1data/l1.min",
             "sol":"levels/l1data/l1.sol",
             "palette":"levels/l1data/l1_2.pal",
             "dun_path":"out/gen/2-271828182.dun",
             "micro_len":10, "blocks":10}}}
```

**使用 `scene.build`（完整场景）**：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"scene.list", "arguments":{}}}
{"jsonrpc":"2.0","id":2,"method":"tools/call",
 "params":{"name":"scene.build",
           "arguments":{
             "id":"tristram",
             "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
             "preview_max_width":1000}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call",
 "params":{"name":"scene.build",
           "arguments":{
             "id":"l1_rnd2",
             "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
             "lighting":true,
             "auto_light":true,
             "preview_max_width":1000}}}
```

`scene.build` 返回 `image_base64`（≤1000px 预览 PNG）、`overlay_base64`（同尺寸 SOL 叠加层
PNG）、`preview_width/height`、`scale`、`width/height`（原生尺寸）、`grid_width/height`、
`transparent_pixel_count`（=0）、`meta`（每格标志数组），并把原生 PNG / 预览 / 叠加层 /
sidecar JSON 写到 `out/scenes/`。原生大图只落盘，WS 只传降采样预览，两层由同一
`SceneProjection` 生成、像素级对齐。

> **地图原图/资源在哪**：MPQ 内没有现成的地图 PNG。城镇是
> `DIABDAT.MPQ` 的 `levels/towndata/town.{cel,til,min,sol,pal}` +
> `sector1s..4s.dun`（以及 `automap.dun`/`automap.amp`）；地牢是
> `levels/lNdata/lN.{cel,til,min,sol}` + 对应 `.dun`。本工具把它们拼装/渲染后写到
> `out/scenes/<id>.png`（原生）、`<id>_preview.png`（≤1000px）、`<id>_overlay.png`
> （SOL 叠加层）和 `<id>.json`（sidecar）。屏幕轴与 DevilutionX 一致：
> `screenX=(x-y)*32`、`screenY=(x+y)*16`（East 增加 X、行增加 Y）。

> **tiles / objects 的标记**：地表瓦片的形状来自 `LevelCelBlock` 高 3 位
> （`Square` / `TransparentSquare` / 左/右三角 / 左/右梯形），碰撞/光照来自 SOL 的
> `TileProperties`（`Solid`、`BlockMissile`、`BlockLight`、`Transparent`、
> `TransparentLeft/Right`）。城镇里的树木/拱门不是普通 tile，而是 `dSpecial` 标记 +
> 独立 `levels/towndata/towns.cel`（64px 宽、224px 高的树冠帧）；地牢的箱子/火把等则是
> DUN 的 object layer + `objects/*.cel`。`scene.build` 已处理 `dSpecial` 与
> `TransparentSquare` 地板（避免把 RLE 当三角形解码产生彩色噪点），并应用 Tristram 的
> 非任务态 `FillTile`（水井/喷泉、传送入口）；地牢 object layer 尚未渲染。

**光照（可选，忠实 dLight）**：`lighting:true` 时按场景默认环境光（城镇 0 全亮、地牢 15
全黑）+ `lights[]` 点光源，用 DevilutionX `LightTables`（`level_light.cpp`）逐格烘焙调色板
暗部；`auto_light` 在无显式光源时于场景中心放一支 radius-15 火把，避免地牢预览全黑。
返回的 `meta.light` 是每格 0..15 光照级别，`ambient` / `light_lit_count` 给摘要。

**寻路（`scene.path`）**：对同一场景 SOL 碰撞网格做 8 方向 A*（直行 100 / 斜行 101，
与 DevilutionX `Source/engine/path.cpp` 一致），返回 `path[]` 的微瓦片坐标与
原生/预览像素坐标，可直接叠在 `scene.build` 的预览上。

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call",
 "params":{"name":"scene.path",
           "arguments":{
             "id":"tristram",
             "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
             "from":{"x":48,"y":48},
             "to":{"x":60,"y":60}}}}
```

**前端场景查看器**：浏览器打开 `web/`（见 README §运行 与 `web/README.md`），切到
第 5 种「场景查看器」→ 选场景 → 加载，可缩放（1–8×）/ 拖拽平移 / 切换 SOL 叠加层 /
夜景提亮 / **光照（dLight 暗部 + 中心火把）** / 复位。场景目录：`tristram`（城镇，96×96）、
`l1_rnd2`、`l1_skngdo`（L1）、`l2_blind`（L2）、`l3_anvil`（L3）、`l4_diab1`（L4，12/16 瓦片）。
「提亮（夜景）」**默认勾选**：Tristram 夜景均值亮度仅 13、67% 像素 <24，不提亮观感近似
空图；CSS 用 `brightness(1.7) contrast(0.85) saturate(1.1)` 抬暗部、菱形背景仍近黑。
预览 PNG 由 `src/decoders/deflate.cpp` 压缩，`scene.build(tristram)` 约 343 KB base64，
远低于 Firefox `data:` URL ~2 MB 上限（早期 stored 块 ≈2.2 MB 会显示「无法显示的图片」）。

**关卡原始 tile 查看器（`web/level.html` + `level.inspect`）**：用于核对
`FillTile`、megatile、micro piece、TileType 与具体 CEL 帧。启动 WS 桥后打开：

```text
http://localhost:8000/level.html
```

页面默认填好 Tristram 的 `levels/towndata/town.{cel,til,min,sol,pal}` 与 `megatile=71`
（水池）；`MPQ 路径` 也已预填 `DIABDAT.MPQ`。点「连接」后会自动检查一次，
并用 `◀ / ▶` 浏览上一个/下一个 megatile。**「图块集」下拉可切换**：
`Tristram 城镇` / `Cathedral L1` / `Catacombs L2` / `Caves L3` / `Hell L4` /
`Hellfire Crypt L5` / `Hellfire Nest L6`，会自动填入对应的 `.cel/.til/.min/.sol/.pal`、
`blocks`、`micro_len` 与主 MPQ（L5/L6 用 `hellfire.mpq`）。每格显示 4 个 micro piece、每个 block 的
`TileType/frame/data`、引用帧 PNG，以及这些 micro 拼出的透明底等距预览。
如果状态栏提示「后端没有 level.inspect」，说明 18765 端口上还是旧进程：停止后重新构建/
重启 `mcp-server --ws 18765` 即可。

> **2026-11-09 修复 tile 纵向翻转**：`DecodeLeft/RightTriangle`、`DecodeLeft/RightTrapezoid`
> 输出的是 **top-down** RGBA，而 `DecodeSquare/TransparentSquare` 输出的是 **bottom-up**
> （源 CEL 行 0 为底），`blitFrame` 又统一做了一次纵向翻转，导致三角形/梯形 tile 被上下翻转。
> 水池的 `RightTrapezoid` 环因此错位、看起来不是封闭圆。现在所有解码器统一输出 top-down，
> `blitFrame` 不再翻转；`unit_tests` 增加 Square 行序回归断言。

- 源码路径：`src/tools/map_tools.cpp`、`src/tools/scene_tools.cpp`、
  `src/decoders/level.h` / `level_decode.cpp` / `level_cel.cpp` / `level_render.cpp` /
  `level_scene.cpp` / `level_light.cpp` / `level_path.cpp`、`src/decoders/png.cpp` /
  `deflate.cpp`、`web/index.html` / `src/main.js` / `styles.css`；
  测试脚本 `out/unit_tests.cpp`、`out/integration_tests.cpp`；管线与已踩坑详见
  `docs/ARCHITECTURE.md` §9.2/§9.3、§10.5。

## 开发文档

更详细的架构与阶段计划见 `docs/ARCHITECTURE.md` 与 `docs/PROJECT.md`；动态地牢生成规则见
`docs/MAP_GENERATION.md`；DiabloUI 界面布局转录、资源清单与调色板分工见 `docs/UI_LAYOUT.md`。
