# 动态地牢生成规则（对照 DevilutionX）

> 目标：解释 Diablo 1 / Hellfire 中**非固定地图**（Cathedral / Catacombs / Caves / Hell /
> Hellfire Crypt / Nest）的生成规则，并给出可落地的获取与渲染路径。
>
> 参考实现：`D:\work\xt\diablo\DevilutionX\Source\levels\`。
> 注意：该 checkout 中部分函数名与上游不同（例如 `drlg_l1.cpp::CreateL5Dungeon()` 实际处理
> `DTYPE_CATHEDRAL` / `DTYPE_CRYPT`），本文以**实际源码调用关系**为准。

---

## 1. 固定地图 vs 动态地图

| 地图 | 来源 | 是否固定 |
|------|------|----------|
| Tristram 城镇 | `town.cpp::DrlgTPass3` + 4 个 `sector*.dun` + `automap.dun` | ✅ 固定 |
| 任务/剧情小关卡 | `levels/l*/...dun`、`nlevels/...dun`（如 `rnd2.dun`、`blind1.dun`） | ✅ 固定预设 |
| Cathedral L1–L4 | `drlg_l1.cpp`（本 checkout 入口 `CreateL5Dungeon`）按种子生成 | ❌ 动态 |
| Catacombs | `drlg_l2.cpp::CreateL2Dungeon` | ❌ 动态 |
| Caves / Nest | `drlg_l3.cpp::CreateL3Dungeon` | ❌ 动态 |
| Hell | `drlg_l4.cpp::CreateL4Dungeon` | ❌ 动态 |
| Hellfire Crypt | `drlg_l1.cpp` 的 Crypt 分支（`DTYPE_CRYPT`） | ❌ 动态 |

因此 `scene.build` 里现有的 `l1_rnd2` / `l2_blind` / … 只是**固定预设房间**，
不是完整的一层地牢。

---

## 2. 入口与数据流

```text
Save / 新游戏
  └─ DungeonSeeds[currlevel]  (uint32, 每层一个)
       │
       ▼
SetRndSeedForDungeonLevel()  ── SetRndSeed(seed)
       │
       ▼
CreateDungeon(seed, entry)   (Source/levels/gendung.cpp:347)
       │  switch (leveltype)
       ├─ DTYPE_TOWN       → CreateTown
       ├─ DTYPE_CATHEDRAL  ┐
       ├─ DTYPE_CRYPT      ┴→ CreateL5Dungeon  (drlg_l1.cpp；实际为 Cathedral/Crypt)
       ├─ DTYPE_CATACOMBS  → CreateL2Dungeon
       ├─ DTYPE_CAVES      ┐
       ├─ DTYPE_NEST       ┴→ CreateL3Dungeon
       └─ DTYPE_HELL       → CreateL4Dungeon
       │
       ▼
   dungeon[40][40]  (1-based megatile id，0=空)
       │  DRLG_LPass3 / Pass3
       ▼
   dPiece[112][112] (0-based micro piece id；四周预填 Dirt/floor)
       │  SetDungeonMicros
       ▼
   DPieceMicros[1379] → level CEL 帧 → 两遍等距渲染
```

- `DungeonSeeds[]` 在存档里按层保存；多人游戏由 `gameGenerator` 生成。
- `LevelSeeds[currlevel]` 用于**重试**：生成循环失败（面积不足 / 放不下楼梯）时，
  保存 LCG 当前状态，下一次从同一状态重来，以得到确定性结果。
- 生成结束后 `Make_SetPC(SetPiece)` 标记 set-piece 区域，防止后续随机内容覆盖。

---

## 3. 随机数规则（必须完全一致）

DevilutionX 为兼容原版使用 **Borland C/C++ LCG**（`Source/engine/random.hpp`）：

```text
state_{n+1} = state_n * 0x015A4E35 + 1   (mod 2^32)
```

`GenerateRnd(v)`：

```text
r = abs((int32_t)state)          // INT_MIN 特判
if (v <= 0) return 0
if (v <= 0x7FFF) return (r >> 16) % v
return r % v
```

`FlipCoin(freq)` = `GenerateRnd(freq) == 0`。
`SetRndSeed(seed)` 同时写入 `sglGameSeed`；`GetLCGEngineState()` 返回 `sglGameSeed`。

**要点**：只实现“平均值随机”不够；任何多取/少取一次随机数都会让后续整张地图完全不同。
逐函数移植时必须保持调用顺序与次数一致。

---

## 4. 各生成器高层规则

### 4.1 Cathedral / Crypt（`drlg_l1.cpp`）

真实生成函数：`GenerateLevel(entry)` → `Pass3()`；本 checkout 入口名为
`CreateL5Dungeon(rseed, entry)`。

关键流程：

1. `SetRndSeed(rseed)`；若 `LevelSeeds[currlevel]` 存在则覆盖。
2. `minarea`：L1=533、L2=693、其它=761（`FindArea()` 不足则重来）。
3. `DRLG_InitTrans` → `FirstRoom` → `FindArea`：
   房间/走廊的布尔网格，`VerticalLayout` 决定横/竖布局。
4. `MakeDmt`：把房间/走廊结果映射成 megatile 的 `dungeon[40][40]`。
5. `FillChambers`、`FixTilesPatterns`、`AddWall`：补房间、修 tile pattern、加墙。
6. `FloodTransparencyValues(13)`：生成 `dTransVal` 透明组。
7. `PlaceStairs(entry)`：放上/下楼梯；失败则整层重来（`LevelSeeds` 保存重试种子）。
8. `FixTransparency`、`FixDirtTiles`、`FixCornerTiles`、`Substitution`、
   `ApplyShadowsPatterns`。
9. Cathedral 额外随机放 5–9 个 `LAMPS` miniset、`FillFloor()`。
10. Crypt 分支：`FixCryptDirtTiles`、`CryptSubstitution`、`InitCryptPieces`、
    `SetCryptSetPieceRoom`、`PlaceCryptLights`。
11. `memcpy(pdungeon, dungeon, ...)`，`DRLG_CheckQuests` 放任务 set-piece。

依赖：`Quests`（任务是否可用决定 set-piece）、`gbIsHellfire`、`DungeonSeeds`、
`dPiece`/`dungeon` 全局、`MiniSet` 表。

### 4.2 Catacombs（`drlg_l2.cpp`）

`CreateL2Dungeon` → `GenerateLevel`：

1. `LevelSeeds[currlevel] = GetLCGEngineState()`；`InitDungeonFlags` + `DRLG_InitTrans`。
2. `CreateDungeon()`：房间（`CreateRoom`）→ 走廊（`ConnectHall`）→ 门（`CreateDoorType`）
   → `FillVoids` / `KnockWalls`。
3. `FixTilesPatterns`。
4. `InitSetPiece()` 按任务状态加载固定房间：
   `Q_BLIND → blind1.dun`、`Q_BLOOD → blood1.dun`、`Q_SCHAMB → bonestr2.dun` 等。
5. `FloodTransparencyValues(3)`、`FixTransparency`。
6. `PlaceStairs(entry)`；失败则用 `LevelSeeds` 重来。
7. `FixDirtTiles`、`DRLG_PlaceThemeRooms(6,10,3,0,false)`、
   大量 `PlaceMiniSetRandom(...)`（门/拱门/柱/拷问架等）。
8. `PlaceStairs` 后 `Pass3` 把 `dungeon` 展开成 `dPiece`。

### 4.3 Caves / Nest（`drlg_l3.cpp`）

`CreateL3Dungeon` → `GenerateLevel`：

1. `InitDungeonFlags`。
2. 房间/区块：`FillRoom`、`CreateBlock`、`FloorArea`、`FillDiagonals`、`FillSingles`、
   `FillStraights`、`Edges`。
3. `MakeMegas` → `River`（洞穴/巢穴的河流/岩浆）。
4. `Spawn`/`SpawnEdge` 扩展洞穴区域。
5. 水池/岩浆池：`PlaceSlimePool` / `PlaceLavaPool` / `PlacePool` / `PoolFix`。
6. 栅栏：`Fence` / `AddFenceDoors` / `FenceDoorFix`。
7. 特殊房间：`PlaceAnvil`、`Warp`、`HallOfHeroes`、`Lockout`。
8. `PlaceStairs`（Caves 与 Nest 分支不同：`PlaceCaveStairs` / `PlaceNestStairs`）。
9. `PlaceLights` / `PlaceCaveLights` / `PlaceHiveLights`。
10. `Pass3`；Nest/Crypt 使用 `DRLG_LPass3` 的 prefill 逻辑。

### 4.4 Hell（`drlg_l4.cpp`）

`CreateL4Dungeon` → `GenerateLevel`：

1. 与 L1 类似的房间/走廊：`FirstRoom`/`GenerateRoom`/`AddWall`/`FixTilesPatterns`。
2. `MirrorDungeonLayout()`：地狱层会镜像布局。
3. `PrepareInnerBorders` / `ProtectQuads` / `LoadDiabQuads`：
   读取 `levels/l4data/diab1.dun`、`diab2a/b.dun`、`diab3a/b.dun`、`diab4a/b.dun`
   等固定 Diablo 房间，保护其区域。
4. `FixTransparency`、`FixCornerTiles`、`CloseOuterBorders`、`GeneralFix`。
5. `PlaceStairs(entry)`；失败重来。
6. `Pass3`（`microLen=12` / `blocks=16`）。

### 4.5 Hellfire Crypt / Nest

- `DTYPE_CRYPT` 走 `drlg_l1` 的 Crypt 分支（`CryptSubstitution`、`InitCryptPieces`、
  `SetCryptSetPieceRoom`、`PlaceCryptLights`）；固定房间 `uberroom.dun` /
  `cornerstone.dun`。
- `DTYPE_NEST` 走 `drlg_l3` 的 Nest 分支（`PlaceNestStairs`、`PlaceHiveLights`、
  `l6` 资源）。

---

## 5. 主题 / 任务 / miniset 依赖

- `Source/levels/themes.cpp`：`THEME_*` 主题房间（图书馆、拷问室、血泉、净化泉、
  泥泉、泪泉、铠甲架、武器架、宝箱等），由 `DRLG_PlaceThemeRooms` 在生成后放置。
- `Source/levels/drlg_quests.cpp`：`DRLG_CheckQuests` 把任务 set-piece 放进地图。
- `Source/levels/setmaps.cpp`：固定剧情关卡（`SL_*`）直接加载 `levels/.../*.dun`。
- `Source/quests.cpp::InitialiseQuestPools`：新版 DevilutionX 默认
  `randomizeQuests = true`，会以 `DungeonSeeds[15]` 为 seed，把
  `{Q_SKELKING,Q_PWATER}`、`{Q_BUTCHER,Q_LTBANNER,Q_GARBUD}`、
  `{Q_BLIND,Q_ROCK,Q_BLOOD}`、`{Q_MUSHROOM,Q_ZHAR,Q_ANVIL}`、
  `{Q_VEIL,Q_WARLORD}` 各随机禁用一个任务。seed 0 时被禁用的是
  `Q_SKELKING / Q_LTBANNER / Q_BLIND / Q_ANVIL / Q_VEIL`。
  这直接影响 set-piece 与楼梯，因此**同一个 dungeon seed 也可能得到不同地图**。
- **结论**：同一个 seed 在不同任务状态下可能得到不同地图；要复现某存档的完整地图，
  必须同时复现 `DungeonSeeds[level]`、`DungeonSeeds[15]`（任务池）与
  `Quests`/`gbIsHellfire` 状态。

---

## 6. 现在如何拿到一层的“完整地图”

### 6.1 推荐：用 DevilutionX 调试版导出（无需先移植生成器）

DevilutionX 的调试构建带 Lua 控制台，`Source/lua/modules/dev/level.cpp` 提供：

- `devilutionx.dev.level.reset(level, seed)`：给某层设置种子并清除已访问标记。
- `devilutionx.dev.level.warp.dungeon(level)`：进入该层（触发生成）。
- `devilutionx.dev.level.exportDun()`：把当前层导出为
  `<currlevel>-<seed>.dun`（写入游戏工作目录；**追加模式**，导出前先删除旧文件）。
- `devilutionx.dev.level.seed(level)`：读取某层种子。

导出格式（`ExportDun`）：

```text
uint16 width=40, uint16 height=40
40×40 uint16 dungeon tile layer
80×80 uint16 padding 层（全 0）
80×80 uint16 怪物层
80×80 uint16 object 层
80×80 uint16 dTransVal 透明层
```

即 **4 个 micro 层**（padding / monster / object / transparency，共 54404 字节）。
固定预设 DUN 通常是 tile-only，城镇 sector DUN 有 4 个 micro 层。DevilutionX 的测试
fixture 使用的是 13 层变体。`loadDun()` 现在统一取**最后一个完整 micro 层**作为
transparency（4 层取 index 3、3 层取 index 2、13 层取 index 12）。

工作流：

1. 用 `-DDEBUG=ON` 构建 DevilutionX 并启动（参考 `DevilutionX/docs/building.md`）。
2. 进入游戏（新角色或读档），打开 Lua 控制台（反向单引号/grave 键）。
3. 依次执行（每层一次，先删除旧的 `<level>-<seed>.dun`）：

   ```lua
   devilutionx.dev.level.reset(1, 123456789)
   devilutionx.dev.level.warp.dungeon(1)
   devilutionx.dev.level.exportDun()
   ```

4. 对 L2–L4 / L5–L6 重复，得到每层 `.dun`。
5. 用本项目的 `map.build` 渲染（新增 `dun_path` 本地文件参数）：

   ```json
   {"jsonrpc":"2.0","id":1,"method":"tools/call",
    "params":{"name":"map.build","arguments":{
      "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
      "cel":"levels/l1data/l1.cel",
      "til":"levels/l1data/l1.til",
      "min":"levels/l1data/l1.min",
      "sol":"levels/l1data/l1.sol",
      "palette":"gendata/cut2.pal",
      "dun_path":"out/gen/1-123456789.dun",
      "micro_len":10, "blocks":10}}}
   ```

   `map.build` 会输出完整 PNG + 预览 + sidecar JSON 到 `out/maps/`。

> 注意：`ExportDun` 的 object 层是 **object id**，monster 层是 **monster id**；
> 当前 `map.build` 只读取 tile 层（+自动识别的 transparency），因此渲染的是
> **地形/墙体/透明组**，不含怪与物件精灵。后续接入 object layer 后即可完整还原。

### 6.2 原生生成（已实现：Cathedral L1–L4）

`src/decoders/dungeon_gen.{h,cpp}` + `src/decoders/dungeon_gen_l1.cpp` 是
`drlg_l1.cpp` Cathedral 分支的逐函数移植，包含：

- Borland LCG（`0x015A4E35` + 1）与 `GenerateRnd` / `FlipCoin` / `DiscardRandomValues`。
- `FirstRoom` / `GenerateRoom`（含原版 height/width 交换 bug）→ `MakeDmt` →
  `FillChambers`（含 3 个 chamber + hall + set piece）→ `FixTilesPatterns` →
  `AddWall` → `PlaceStairs` → `FixDirtTiles`/`FixCornerTiles` → `Substitution` →
  `ApplyShadowsPatterns` → `LAMPS` → `FillFloor`。
- 原版重试循环：面积 `< minarea`（L1 533 / L2 693 / 其它 761）或楼梯放不下时，
  继续使用当前 LCG 状态重来（不是重置 seed）。
- 任务 set-piece：`rnd6.dun`（Butcher）/ `skngdo.dun`（Skeleton King）/
  `banner2.dun`（Ogden's Sign）；`L5STAIRSUP`（`originalCathedral` 且无 banner）。
- **任务可用性必须按 `Quest::IsAvailable()` 计算**：`setlevel == 0`、
  `currlevel == _qlevel`、`_qactive != QUEST_NOTAVAIL`、
  多人模式下排除 `isSinglePlayerOnly`。
  注意新版 DevilutionX 默认开启 `randomizeQuests`，会用 `DungeonSeeds[15]` 的
  seed 通过 `InitialiseQuestPools` 把若干任务标为 `QUEST_NOTAVAIL`
  （seed 0 时是 `Q_SKELKING` / `Q_LTBANNER` / `Q_BLIND` / `Q_ANVIL` / `Q_VEIL`）。
  复现某张地图时必须使用同一任务状态；本项目的 `level.generate` 用
  `quests: "vanilla" | "none" | "all"` 表达常见组合。

MCP 工具：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"level.generate","arguments":{
   "archive":"D:/work/xt/diablo/DevilutionX-Hell Fire/DIABDAT.MPQ",
   "level":2, "seed":271828182,
   "original_cathedral":true, "quests":"vanilla",
   "out":"out/gen"}}}
```

返回 `dun_path`（ExportDun 兼容，4 个 micro 层）、`tiles`（40 行 hex）以及
`floor_count` / `stairs_up_count` / `stairs_down_count`。随后用 `map.build` 的
`dun_path` 渲染；**Cathedral L1–L4 共用 `levels/l1data/l1.{cel,til,min,sol}` +
`gendata/cut2.pal`（`micro_len=10` / `blocks=10`）**，不要误用 `l2data/l3data/l4data`
（那些是 Catacombs/Caves/Hell）。

验收（已通过）：DevilutionX 仓库自带的 `test/fixtures/{diablo,hellfire}/*.dun`
共 **14 个 Cathedral fixture**（Diablo + Hellfire，含 Butcher/Skeleton King/
Ogden's Sign/Poisoned Water 各种任务组合）与 `generateCathedral()` 输出
**40×40 全格 0 差异**。单元测试会读取
`DHFC_DRLG_FIXTURES`（默认 `D:/work/xt/diablo/DevilutionX/test/fixtures`），
目录不存在时自动 SKIP。

> set-piece 文件存在两个版本：零售 MPQ 与 DevilutionX 仓库
> `test/fixtures/levels/l1data/`（后者是对齐测试基准的修正版）。要逐格复现
> fixture，必须使用 fixture 目录中的 set-piece；`level.generate` 默认从
> `archive`/`archives` 搜索栈读取，与运行时 mod 覆盖规则一致。

后续仍需移植：

1. **Catacombs（`drlg_l2.cpp`，L5–L8）**：房间/走廊/门 + `blind1`/`blood1`/
   `bonestr2` set-piece + theme rooms。
2. **Caves / Nest（`drlg_l3.cpp`，L9–L12 / Hellfire L6）**：区块/河流/水池/
   栅栏/`anvil` 等。
3. **Hell（`drlg_l4.cpp`，L13–L16）**：镜像布局 + `diab1..4` 固定房间保护。
4. **Crypt（`drlg_l1.cpp` Crypt 分支，Hellfire L5）**：`uberroom`/`cornerstone`。
5. `dTransVal` 的 flood-fill 也可按同样方式移植，以让生成 DUN 的透明遮挡与
   导出文件完全一致（当前生成文件该层为 0）。

验收标准：同一 seed + 同一任务状态下，生成器输出的 `dungeon[40][40]`（以及
`dPiece` 的 2× 展开）与 DevilutionX `ExportDun` 的 tile 层 **0 差异**。

---

## 7. 当前状态与限制

- ✅ 固定地图、预设 `.dun`、完整场景渲染、光照、寻路、tile 查看器。
- ✅ `map.build` 支持 `dun_path`，可直接渲染 DevilutionX 导出的动态地图。
- ✅ **Cathedral L1–L4 原生生成**：`level.generate` 可按 seed 生成正确地图，
  14 个 DevilutionX fixture 全部 0 差异（Diablo + Hellfire 任务组合）。
- ⏳ Catacombs / Caves / Hell / Crypt / Nest 生成器尚未移植（规则见 §4）。
- ⏳ 导出 DUN 的 monster/object 层尚未渲染；object 层接入后箱子/火把/桶等即可显示。
- ⏳ 生成 DUN 的 `dTransVal` 层当前为 0；`dTransVal`/`TransList` 的运行时遮挡
  仍未接入静态渲染。
