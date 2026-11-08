# Diablo 1 / Hellfire 运行时动态文件名规则

> 本文档描述 Diablo 1（Hellfire 版本）MPQ 中**大量运行时动态拼出来**的文件名规则。
> 这些文件没有配套的 `(listfile)`，因此无法从块表直接取得真实名字，必须依据
> DevilutionX 源码中的拼接公式 + 数据表（TSV）反向推导候选名，再用 StormLib 的
> 哈希校验逐个验证（见 `mpq-info/recover_phase0.cpp`）。
>
> 文档与代码同源：下方每个家族都标注了 DevilutionX 的源码位置、`recover_phase0.cpp`
> 中的生成函数、以及已验证的真实扩展名。

---

## 1. 背景：为什么这些文件名需要"推导"

原版 Diablo 1 / Hellfire 的 MPQ **不带 `(listfile)`**。块表（block table）里只有
"文件存在 + 大小 + 加密 key" 等信息，真实文件名要靠 **哈希表（hash table）的 key**
反查。而游戏在运行时会用形如 `plrgfx\{hero}\{prefix}\{prefix}{type}.cl2` 的字符串
**现场拼接**出精灵路径，再交给 `SFileOpenFileEx` 打开。

因此"确认每个文件作用"的研究阶段，核心工作就是：

1. 从 DevilutionX 源码定位**拼接公式**（`BufCopy(out, "plrgfx\\", ...)` 这类）。
2. 从对应数据表（`*.tsv`）提取公式里所需的**变量值**（职业、怪物后缀、精灵基名…）。
3. 枚举所有组合生成**候选名**，喂给 `recover_phase0.cpp` 逐档案校验，命中即证明
   该名是真实存在的文件。

涉及的六大运行时拼接家族：**plrgfx / monsters / missiles / items / objects / towners**。
（另有一类紧密相关的 **TRN 调色板** 文件，见第 8 节。）

---

## 2. 通用恢复机制

```text
候选名 ──► StormLib::SFileOpenFileEx(archive, name)  ──► 成功 = 真实文件
                                                     └─► 失败 = 候选名不正确
```

- 哈希表是单向的：能打开 ⇒ 名字真实存在；打不开 ⇒ 名字不对（不会误报）。
- 一个名字只能解析到恰好一个文件，故**候选名解析结果可信**，无伪造风险。
- `recover_phase0.cpp` 对每一档案：读候选集 → 逐个 `verify()` + `querySize()` →
  落盘 `mpq-info/dict/recovered/<档案>.txt`（每行 `name<TAB>size`）并汇总
  `recovered.json`。

---

## 3. plrgfx — 角色精灵（Warrior / Rogue / Sorcerer / Monk / Barbarian / Bard）

**公式**（`Source/player.cpp:1506`，`GetPlayerGraphicsPath`）：

```text
plrgfx\{classPath}\{c}{a}{w}\{c}{a}{w}{type}.cl2
```

| 片段 | 取值 | 来源 |
|------|------|------|
| `classPath` | `warrior` `rogue` `sorcerer` `monk` `barbarian` `bard` | 各 `assets/txtdata/classes/<c>/sprites.tsv` 的 `classPath` 字段 |
| `c`（classChar） | `w` `r` `s` `m` `c` `b` | 同上 `classChar` 字段（`warrior=w` … `bard=b`） |
| `a`（ArmourChar） | `l` `m` `h` | 固定：`l`=light `m`=medium `h`=heavy（`player.h:170`） |
| `w`（WepChar） | `n` `u` `s` `d` `b` `a` `m` `h` `t` | 固定：unarmed / no-weapon+shield / sword / sword+shield / bow / axe / blunt / blunt+shield / staff（`player.h:176`） |
| `type` | `as` `st` `aw` `wl` `at` `ht` `lm` `fm` `qm` `dt` `bl` | `player.cpp:2098` 的 `switch (graphic)` |

**动作 → type 映射**（`player.cpp:2098`）：

| 动作 | type | 备注 |
|------|------|------|
| Stand（地牢） | `as` | |
| Stand（城镇） | `st` | `leveltype == DTYPE_TOWN` |
| Walk（地牢） | `aw` | |
| Walk（城镇） | `wl` | |
| Attack | `at` | 仅地牢 |
| Hit | `ht` | 仅地牢 |
| Lightning | `lm` | |
| Fire | `fm` | |
| Magic | `qm` | |
| Death | `dt` | **强制武器为 Unarmed（`w`=`n`）** |
| Block | `bl` | 仅地牢且需 `_pBlockFlag` |

> 示例：`plrgfx\rogue\rha\rhaas.cl2` = 女盗贼（rogue）+ heavy 护甲（h）+ axe（a）+ Stand（as）。

- **生成函数**：`genPlayers()`（遍历 `classes/*/sprites.tsv`，三重循环 armour×weapon×type）。
- **已验证扩展名**：`.cl2`（DevilutionX 在加载时把 CL2 转 CLX，MPQ 内存储为 `.cl2`）。

---

## 4. monsters — 怪物精灵

**公式**（`Source/monster.cpp:3221`，`LoadMonsterSpritesData`）：

```text
monsters\{assetsSuffix}{L}.cl2      L ∈ "nwahds"
monsters\{trnFile}.trn              （怪物色彩 / 唯一怪物调色板）
```

- `assetsSuffix` 来自 `assets/txtdata/monsters/monstdat.tsv` 的 **`assetsSuffix` 列**
  （第 3 列）。注意该值本身可能含子目录，如 `acid\acid`、`antworm\worm`，需原样拼接。
- `Animletter[7] = "nwahds"`（`monster.cpp:152`）→ `L` 取 `n w a h d s` 六个字母
  （对应 n/walk? a/h/d/s 为各动画帧组）。
- `trnFile` 来自 `monstdat.tsv` 的 **`trnFile` 列**（第 5 列），生成 `monsters\{trnFile}.trn`
  （怪物色彩 TRN，约 94 个，如 `monsters\acid\acidb.trn`、`monsters\bat\grey.trn`）。
- Hellfire 额外怪物见 `mods/hf/txtdata/monsters/monstdat.tsv`（需合并去重）。

- **生成函数**：`genMonsters()`。
- **已验证扩展名**：`.cl2`（精灵）/ `.trn`（调色板）。

---

## 5. missiles — 导弹 / 特效精灵

**公式**（`Source/tables/misdat.cpp:420`，`MissileFileData::LoadGFX`）：

```text
missiles\{name}.clx        （UNPACKED_MPQS 分支）
missiles\{name}            （animFAmt==1，按 .cl2 加载，无扩展名）
missiles\{name}.cl2        （animFAmt!=1，FileNameGenerator + DEVILUTIONX_CL2_EXT）
```

- `name` 取自 **`assets/txtdata/missiles/missile_sprites.tsv`** 的 **`name` 列**
  （注意：这是**精灵基名**，不是 `misdat.tsv` 的法术名）。
- Hellfire 额外见 `mods/hf/txtdata/missiles/missile_sprites.tsv`。
- 实际 MPQ 内存储为 `.cl2`（非 `.clx`）；生成时同时给出 `.cl2` 与无扩展名两种候选。

- **生成函数**：`genMissiles()`。
- **已验证扩展名**：`.cl2`。

**编号文件规则（2026-11-08 实测修正）**：`missile_sprites.tsv` 的 `numFrames` 是
**编号文件个数**，`frameLength[i]` 是该文件内帧数。`FileNameGenerator` 的 `min=1`，
因此：

```text
numFrames > 1  ->  missiles\{name}1.cl2 … missiles\{name}{numFrames}.cl2
numFrames == 1 ->  missiles\{name}.cl2        （无数字后缀；{name}1.cl2 不存在）
```

实测 `Fireball numFrames=16` → `fireba1..16.cl2` 全部存在、各 14 帧；
`FireWall=2` → `firewal1/2.cl2`（13/11 帧）；`Lightning=1` → `lghning.cl2`。
这些编号文件**不在** `recovered.json`（只恢复了 33 个单帧 missiles 名），由
`spell.list` 从数据表合成清单发现。**编号文件是方向而非连续动画帧**：
`LoadGFX` 用 `LoadMultipleCl2Sheet<16>(pathGenerator, animFAmt, animWidth)` 把
`animFAmt` 个文件拼成 `ClxSpriteSheet`，运行时
`spritesForDirection(getDirection16())` 取 `sheet()[direction]`，`animLen(dir)`
给出该方向的帧数（即 `frameLength[i]`）。因此技能/战斗查看器播放时只取当前方向
对应的那一份编号文件，不把 16 个文件平铺成一条跨越方向的动画。

**宽度必须显式传**：`sprite_width.cpp` 的 missiles 分支按 basename 精确查
`missile_sprites.tsv` 的 `name` 列，`fireba1` 查不到表里的 `fireba`，故
`sprite.decode(name:"missiles\\fireba1.cl2", width:"auto")` 会报
`auto width failed`。应从 `missile_sprites.width`（96/128/160…）取显式整数宽度；
`spell.list` 的 `sprite` 层已带 `width` 字段。

---

## 6. items — 物品掉落 / 背包精灵

**公式**（`Source/items.cpp:2428`，`InitItemGFX`）：

```text
items\{ItemDropNames[i]}.cel
```

- `ItemDropNames` 是 `items.cpp:230` 的 **44 项固定游戏常量数组**（非 TSV），如
  `armor2` `axe` `bow` `helmut` `mace` `shield` `ring` `scroll` `food` … `mooses1`。
- 另有两处 HUD 静态精灵（同为 `.cel`）：`items\duricons`（`control_panel.cpp:406`）、
  `items\map\mapztown`（`doom.cpp:25`）。
- 通过 `LoadCel` 加载 ⇒ 扩展名 `.cel`。

- **生成函数**：`genItems()`（内嵌 44 常量数组 + 2 个 HUD 精灵）。
- **已验证扩展名**：`.cel`。

---

## 7. objects — 地牢场景 / 互动物件

**公式**（`Source/objects.cpp:3720`，`LoadLevelObjects`）：

```text
objects\{ObjMasterLoadList[i]}.cel
```

- `ObjMasterLoadList` 由 `Source/tables/objdat.cpp:280` 从
  **`assets/txtdata/objects/objdat.tsv`** 的 **`file` 列**（第 2 列）去重填入。
- 经 `LoadCelWithStatus` 加载（CEL 格式）。
- 共 62 个唯一 `file` 值，如 `barrel` `chest1` `angel` `l1doors` `cruxsk1` …。

- **生成函数**：`genObjects()`。
- **已验证扩展名**：`.cel`。

---

## 8. towners — 城镇 NPC 精灵

**公式**（`Source/towners.cpp:130`，`LoadTownerAnimations`）：

```text
towners\{animPath}.cel
```

- `animPath` 来自 **`assets/txtdata/towners/towners.tsv`** 的 **`animPath` 列**
  （该列已含完整路径，如 `towners\girl\girlw1`、`towners\smith\smithn`）。
- 另有若干硬编码变体（`towners.cpp` 字面量）：`farmer\cfrmrn2`、`farmer\mfrmrn2`、
  `girl\girls1`、`animals\cow`。
- Hellfire 见 `mods/hf/txtdata/towners/towners.tsv`。
- 经 `LoadCel` 加载 ⇒ 扩展名 `.cel`。

- **生成函数**：`genTowners()`。
- **已验证扩展名**：`.cel`。

---

## 9. 关联：TRN 调色板（256 字节色彩变换表）

TRN 文件不是独立拼接族，而是上述精灵的**可选调色板叠加**，命名规律如下：

| TRN 家族 | 公式 / 来源 | 生成函数 |
|----------|------------|----------|
| 字体调色板 | `fonts\{name}.trn`（固定清单：`text_render.cpp`：black/white/red/blue/orange/yellow/buttonface/…） | `genFontTrn()` |
| 职业调色板 | `plrgfx\{sprites.tsv 的 trn 字段}.trn`；字面量 `plrgfx\infra.trn` / `plrgfx\stone.trn` / `gendata\pause.trn` | `genClassTrn()` |
| 玩家每精灵变体 | `plrgfx\{class}\{prefix}\{prefix}{type}.trn`（`GetPlayerGraphicTRN` 构造 `pszName+".trn"`） | `genPlayers()` 内追加 |
| 怪物色彩 | `monsters\{trnFile}.trn`（同第 4 节，已覆盖） | `genMonsters()` |

> 注意：TRN 文件**不单独占满候选集**——多数精灵并不存在对应 TRN（加载时可选应用）。
> 当前 `recover_phase0.cpp` 生成的 TRN 候选若解析失败即被过滤，不影响正确性。

---

## 10. 扩展名速查（已逐项验证）

| 家族 | 真实扩展名 | 说明 |
|------|-----------|------|
| plrgfx | `.cl2` | DevilutionX 加载时转 CLX |
| monsters（精灵） | `.cl2` | 同上 |
| monsters（色彩 TRN） | `.trn` | |
| missiles | `.cl2` | |
| items | `.cel` | `LoadCel` |
| objects | `.cel` | `LoadCelWithStatus` |
| towners | `.cel` | `LoadCel` |
| 字体/职业/玩家 TRN | `.trn` | |

> 早期 harvest 字典里曾误写为 `.clx`（如 `plrgfx\monk\mha\mhaas.clx`），经验证
> **不在这些 MPQ 中**；正确扩展名为 `.cl2`（角色/怪物/导弹）或 `.cel`（物品/物件/NPC）。

---

## 11. 与 `recover_phase0.cpp` 的映射

| 家族 | 生成函数 | DevilutionX 数据来源 |
|------|----------|----------------------|
| plrgfx | `genPlayers` | `classes/*/sprites.tsv`（`classPath`/`classChar`）+ `player.h` 固定数组 + `player.cpp:2098` |
| monsters | `genMonsters` | `monstdat.tsv`（`assetsSuffix` / `trnFile`）+ `monster.cpp:152` |
| missiles | `genMissiles` | `missile_sprites.tsv`（`name`） |
| items | `genItems` | `items.cpp:230` 固定数组（44 项）+ 2 个 HUD 字面量 |
| objects | `genObjects` | `objdat.tsv`（`file`） |
| towners | `genTowners` | `towners.tsv`（`animPath`）+ 4 个 `towners.cpp` 字面量 |
| TRN | `genFontTrn` / `genClassTrn` / `genPlayers` 内 | `text_render.cpp` 清单 / `sprites.tsv` `trn` 字段 |

运行与产物见仓库根 `README.md` §测试 第 4 项。当前（阶段 0）累计恢复 **2,080** 个
真实文件名，其中 TRN 调色板约 115 个。

---

## 12. 已知限制 / 后续可扩展

- **唯一怪物 TRN** `monsters\monsters\{trnName}.trn`（`towners`? 实为 `monster.cpp:3324`）：
  依赖 `UniqueMonstersData[].mTrnName`，而 `monstdat.tsv` 中对应 `trn` 列在
  本数据集为空，故当前 MPQ 内无此类独立文件；若后续数据集含唯一怪物可补此规则。
- **DiabloUI 调色板**、**字体 `fonts\*.trn` 之外的 UI TRN** 等极个别拼接族尚未纳入。
- 候选名均为"正向枚举"，属于**穷举 + 哈希校验**；若某资源命名完全脱离上述公式，
  仍需借助 listfile 或爆破法补充。
