# DiabloUI 界面布局还原参考（阶段 5 / 目标 4）

> 本文是阶段 5「还原 `DiabloUI/` 的全部界面布局与元素 + 输出界面布局数据与可视化」的
> 转录依据与资源清单。权威参考为 `D:\work\xt\diablo\DevilutionX\Source\`（`DiabloUI/`、
> `control/`、`inv.cpp`、`panels/`、`stores.cpp`、`engine/palette.cpp`）。
> 结构化数据与查询在 `src/tools/ui_layout.h`（header-only 纯数据，18 屏）；渲染在
> `src/tools/ui_render.h`；MCP 工具在 `src/tools/ui_tools.cpp`；前端第 6 种浏览模式在
> `web/`。工具用法见根 `README.md`「界面布局查看器」与 `docs/ARCHITECTURE.md` §11。

---

## 1. 坐标系与分区

游戏逻辑 UI 恒为 **640×480**（`Source/utils/display.cpp` `constexpr Size UISize{640,480}`；
`GetUIRectangle().position = ((gnScreenWidth-640)/2, (gnScreenHeight-480)/2)`）。所有
`ui_layout.h` 里的 `Rect` 都是**绝对 640×480 坐标**。DevilutionX 中相对分区原点的值另存
`Element.panel` + `Element.panel_rect`，便于对照源码。

| 分区 | 绝对矩形 | 用途 |
|------|----------|------|
| `MainPanel` | `{0,352,640,128}` | 底部控制面板（血蓝球、按钮、腰带、快捷法术、信息框） |
| `LeftPanel` | `{0,0,320,352}` | 角色属性面板、任务日志、网格商店、储藏箱 |
| `RightPanel` | `{320,0,320,352}` | 物品栏/装备槽、法术书 |

> 注：DevilutionX 现行代码用 `Rectangle` + `GetPanelPosition(UiPanels::X)`，已无老版
> `PANEL_LEFT/TOP/WIDTH/HEIGHT` 宏；等价数值由 `CalculatePanelAreas()` 推出
> （`MainPanelSize{640,128}`、`SidePanelSize{320,352}`）。

`validateScreen` 用 `validateRectAnchor`（正尺寸 + 左上角在画布内）校验：DevilutionX 的
文本/对齐矩形可能合法地越出 640 宽并在渲染时裁剪（如 `selok` 正文 `x=140,w=560` →
右边缘 700）。像素（美术）框则在 `ui_render::blit` 处统一裁剪。

---

## 2. 两套调色板（关键实测结论）

MPQ 实测（`mpq.verify` + 逐资源解码 + 像素分布统计）发现 **两类 256 色调色板并存**，
`ArtRef.palette` 是**逐资源权威字段**，`ui.build` 按名分别加载（palette bank）：

| 调色板 | 所在档案 | 用途 | 误用后果 |
|--------|----------|------|----------|
| **内嵌调色板**（PCX 文件尾 `0x0C` + 768B） | `DIABDAT.MPQ` | 所有菜单/对话框 `.pcx`（`ui_art\*.pcx`） | — |
| `ui_art\diablo.pal` | `devilutionx.mpq` | **菜单 / CLX 转换调色板**：`dvl_lrpopup.clx`、`dvl_but_sml.clx` 等 DevilutionX 转换资源 | 低索引为纯原色（`#bf0000/#00bf00/...`），套原版游戏内 `.cel` 会出**彩虹噪点** |
| `levels\towndata\town.pal` | `DIABDAT.MPQ` | **游戏内调色板**（`LoadRndLvlPal(DTYPE_TOWN)`）：`ctrlpan\*.cel`、`data\*.cel`、游戏内面板 `charbg.clx`/`stash.clx` | 用 `diablo.pal` 会导致血蓝球/面板/物品栏出彩虹噪点 |

rainbow 像素占比实测（`max-min>160 且 max>200` 的高饱和纯色）：

| 资源 | `diablo.pal` | `town.pal` |
|------|--------------|------------|
| `ctrlpan\panel8.cel`（控制面板底图 640×144） | 35.0% | **0.0%** |
| `ctrlpan\p8bulbs.cel`（血蓝球 88×88×2） | 65.9% | **0.0%** |
| `data\inv\inv.cel`（物品栏底图 320×352） | 77.7% | **0.0%** |
| `data\charbg.clx`（角色面板底图 320×352） | 59.7% | **0.0%** |
| `data\stash.clx`（网格商店底图 320×352） | 79.1% | **0.0%** |
| `ui_art\dvl_lrpopup.clx`（对话框） | 0.0%（**正确**） | 0.0% |
| `ui_art\dvl_but_sml.clx`（按钮） | 0.0%（**正确**） | 0.0% |

UI 美术跨档案：`DIABDAT.MPQ` 存原版 `.pcx/.cel` + `town.pal`，`devilutionx.mpq` 存
`diablo.pal` + `.clx` + `fonts\*.trn`。`ui.build` 必须传全 MPQ 栈（`archive` + `archives`）。

`CtrlPan` 主菜单背景 `ui_art\mainmenu.pcx` 本身**近乎全黑**（实测最大通道值 12）——这是
真实游戏美术：亮色 Diablo 动画 logo（`ui_art\smlogo`/`hf_logo2`，运行时 `UiAddLogo`）与
金色菜单文字是独立叠加层。本实现文字由前端近似层绘制，动画 logo 明确不实现（见 §6.1）。

---

## 3. 美术资源清单（实测存在性与尺寸）

`ui_art\*.pcx`（内嵌调色板，`DIABDAT.MPQ`）：`mainmenu`(640×480)、`title`、`credits`、
`selhero`、`selgame`、`swmmenu`、`logo`(550×3240 为 15 帧竖排 sprite-sheet，取首帧)、
`smlogo`、`heros`(180×304)、`focus`/`focus16`/`focus42`、`cursor`、`but_sml`、
`sb_bg`/`sb_thumb`/`sb_arrow`、`prog_bg`/`prog_fil`、`r1_gry`、`spopup`(284×148)、
`srpopup`、`lpopup`、`lrpopup`(392×284)。

`ctrlpan\*.cel`（`town.pal`，`DIABDAT.MPQ`）：`panel8`(w640 → 640×144)、
`p8bulbs`(w88 → 88×176，2 帧 88×88 = 血/蓝球)、`panel8bu`(w71)、`p8but2`(w33)、
`golddrop`(w261)、`spelicon`（**帧宽未恢复 → `needs_width`**）、`talkpanl`、`talkbutt`。

`data\*.cel`（`town.pal`，`DIABDAT.MPQ`）：`textbox2`(w271)、`textslid`(w12, 14 帧)、
`quest`(w320)、`spellbk`(w320)、`spellbkb`(w76/61)、`spelli2`(w37)、`charbut`（帧宽未恢复）、
`inv\inv.cel`(w320)、`data\boxleftend/boxmiddle/boxrightend`、`data\hf_logo3` 等。

`.clx` / `.pal`（`devilutionx.mpq`）：`ui_art\diablo.pal`(768B)、
`ui_art\mainmenuw.clx`(1280×480 宽屏，延后)、`ui_art\creditsw.clx`、`ui_art\hf_titlew.clx`、
`ui_art\supportw.clx`、`ui_art\dvl_lrpopup.clx`(385×280)、`ui_art\dvl_but_sml.clx`(110×28×2)、
`data\charbg.clx`(320×352)、`data\stash.clx`(320×352)、`data\panel8buc*.clx`、
`data\boxmiddle.clx`、`data\xpbar.clx`、`fonts\*.trn`(16 个 256B 颜色表)、
位图字体 `fonts\{12,22,24,30,42,46}-00.clx`（**仅基本 ASCII 行**）。

> 未恢复/未纳入：`data\store.clx`（网格商店底图，用 `data\stash.clx` 替代）、
> `data\spelli2_fg/bg.clx`、`data\spelicon_*.clx`、`data\inv\objcurs*.clx`、
> `data\repairAllBtn/repairSingleBtn/tabBtnUp.clx`、宽屏 `*w.clx` 布局、联机
> `selconn`/`selgame` 菜单。这些进 `needs_width`（缺帧宽的 CEL）或直接不引用。

---

## 4. 18 屏布局目录（`ui_layout.h::screens()`）

| category | id | 标题 | 主要元素 / 源码 |
|----------|----|------|-----------------|
| menu | `mainmenu` | 主菜单 | 背景 `mainmenu.pcx` + 5 项金色菜单 `UiList`(64,192,510,43) + 版本行 · `mainmenu.cpp:64-82` |
| menu | `title` | 标题画面 | `title.pcx` + Logo(`logo.pcx` 首帧, y+182) + 版权行 · `title.cpp` |
| menu | `credits` | 制作群 | `credits.pcx` + 滚动文字视口(31,114,580,251) · `credits.cpp:41` |
| menu | `selhero` | 选择英雄 | `selhero.pcx` + 标题 + 头像 + 5 属性行 + 6 行列表 + 滚动条 + OK/Delete/Cancel · `hero/selhero.cpp` |
| menu | `selgame` | 难度选择 | `selgame.pcx` + 标题 + 3 行列表 + OK/Cancel · `selhero.cpp` |
| menu | `selok` | OK 确认 | 标题(24,161,590,35) + 正文 + OK(230,390,180,35) · `selok.cpp` |
| menu | `selyesno` | Yes/No | 黑底 + 标题 + 正文 + Yes/No · `selyesno.cpp` |
| menu | `settings` | 设置 | 标题 + 设置项列表 + 说明 + 滚动条 · `settingsmenu.cpp` |
| hud | `control_panel` | 底部控制面板 / 血蓝球 | `panel8.cel` + 信息框 + 2 血蓝球 + 血/蓝数值 + 腰带 8 格 + 6 宽按钮 + 2 小按钮 + 快捷法术 · `control_panel.cpp` / `control_flasks.cpp` |
| panel | `char_panel` | 角色属性面板 | `charbg.clx` + 名字/职业 + Str/Mag/Dex/Vit + Gold + Life/Mana + 4 个 +按钮 · `charpanel.cpp` |
| panel | `inventory` | 物品栏 / 装备槽 | `inv\inv.cel` + 头/双戒/护符/双手/胸甲 7 槽 + 背包 10×4 + 腰带 8 格 · `inv.cpp InvRect[]` |
| panel | `spellbook` | 法术书 | `spellbk.cel` + 页签 + 法术图标网格 4×3 · `spell_book.cpp` |
| panel | `quest_log` | 任务日志 | `quest.cel` + 任务标题 + 10 行文本 · `quests.cpp` |
| store | `store_list` | 商店（文本列表） | 暗底 + `textbox2` + 20 行商品 + 滚动条 + 金币行 · `stores.cpp` |
| store | `store_grid` | 商店（网格视图） | `stash.clx`(替代 `store.clx`) + 10×9 网格 + 页签/修理按钮 + 金币 · `qol/visual_store.cpp` |
| dialog | `dialog_small` | 小对话框 | `spopup.pcx`(180,168,284,148) + 正文 + OK · `dialogs.cpp` |
| dialog | `dialog_large` | 大对话框 | `lrpopup.pcx`(127,100,385,280) + 标题 + 正文 + OK · `dialogs.cpp` |
| dialog | `dialog_large_dvl` | 大对话框（CLX） | `dvl_lrpopup.clx`(385×280) + 标题 + 正文 + OK · `dialogs.cpp:65` |

关键坐标（绝对 640×480）：
- 血蓝球：`p8bulbs.cel` 88×88 两帧，血球穹顶 X≈107、下半 X≈96；蓝球 475/464（`control_flasks.cpp`）；
  布局取血球 `{96,360,88,88}`、蓝球 `{464,360,88,88}`。
- 腰带：`BeltRect(205,5,232,28)` 相对 MainPanel → 绝对 `{205,357,232,28}`，8 格步长 29。
- `MainPanelButtonRect[]` char/quests/map/menu/inv/spells 命中框 71×20；贴图 `panel8buc.clx` 63×19
  （命中框与贴图框分列）。
- 对话框：`spopup` 280×144、`lpopup/lrpopup` 385×280（由 `dvl_lrpopup.clx` 帧头与
  `progress.cpp GetCenterOffset` 反推）；OK 按钮 `DialogButtonWidth/Height = 110×28`。
- 背包 `InvRect[7..46]` 起点(17,222) 步长 29（相对 RightPanel → 绝对 x+320）。

颜色/字号（`UiFlags`，`ui_flags.hpp` / `text_render.cpp`）：
- `FontSizes={12,24,30,42,46,22}` → `GameFont12/24/30/42/46/FontSizeDialog`；
  `LineHeights={12,26,38,42,50,22}`。
- `text_color` ↔ `ColorTranslations[19]` ↔ `fonts\*.trn`：`goldui`/`grayui`/`golduis`/
  `grayuis`/`yellow`/`white`/`whitegold`/`red`/`blue`/`orange`/`black`/`buttonface`/
  `buttonpushed`/`gamedialogwhite`/`gamedialogyellow`/`gamedialogred` 等。
- 前端 `guiColor` 把这些 TRN 名映射为代表色（近似），`guiFontSize` 把字号映射为 CSS px。

---

## 5. 工具契约与产出

- `ui.list {category?}` → `{count,canvas_default,categories,screens[]}`（无需 MPQ）。
- `ui.layout {id}` → 单屏完整布局 JSON（无需 MPQ），文字字段供前端绘制。
- `ui.build {id,archive,archives?,out?,palette?,overlay?,label_numbers?,background?,skip_missing_art?}`
  → 真实 MPQ 美术合成 640×480 PNG（不透明背景、0 透明像素）+ 对齐元素框叠加层 +
  base64 + 每元素 `decode_status`/`decoded_size`/`placed_rect` + 每资源 `art[]` 状态 +
  `palettes[]` + `needs_width` + 计数；写 `out/ui/<id>.png`、`_overlay.png`、`.json`。

`decode_status` 枚举：`ok`（真实美术解码并贴入）、`text`（无后端美术，前端画文字/框）、
`missing`（抽不到，跳过保留框）、`width_unknown`（CEL 缺帧宽，进 `needs_width`，跳过不伪造）、
`decode_error`（调色板缺失/解码失败）。

---

## 6. 不实现项与明确延后

### 6.1 界面动画播放：明确不实现（项目取舍）

界面布局查看器**不提供「播放动画」按钮**，界面里的动画元素只作静态单帧呈现，不播放序列帧。
典型例子是**标题画面 / Logo** 的 diablo 字样火焰：DevilutionX `title.cpp:43` 用
`LoadPcxSpriteList("ui_art\\logo", 15, 250)` 把 `ui_art\logo.pcx`（550×3240，15 帧竖排、
每帧 550×216）切成 15 帧，交给 `UiImageAnimatedClx` 按 `GetAnimationFrame(15)`（约
16.7fps）循环播放（资源清单见 §3）。

原因（既定方案，不为此改动）：`ui.build` 在**后端**把整屏（背景 + 面板 + 元素美术）离屏
合成**一张静态 640×480 PNG** 返回前端，前端只有单张 `#guiBase` 画布 + 独立的文字层 /
元素框叠加层；这套「后端整屏合成 → 前端 blit」是阶段 5 的既定分层。要播放动画就必须改
分层或把合成移到其它层，属于方案级调整，本项目**不为此改动**，也不新增工具、不改
`ui.build` 的输出契约。

2026-11-10 评估后**明确放弃**该需求（不是「延后」，不再排期）。`ui_layout.h` 的
`ti.logo` 保持现状静态呈现。若以后重开此需求，须先确认方案再动手。

### 6.2 明确延后

- **游戏位图字体逐字渲染**：`fonts\{size}-{row:02X}.clx` 在 MPQ 仅恢复基本 ASCII 行 00、
  中文/扩展字符缺字；后端不烧录文字，前端用系统字体**近似**绘制（可关闭核对纯美术底图）。
- 动态文字运行时绑定（占位符目前替换为示意值）、按钮 hover/按下帧、聚焦框 focus 动画。
- 宽屏 1280×480 CLX 布局（`mainmenuw.clx` 等）、联机 `selconn`/`selgame` 菜单。
- 未恢复资源名（`data\store.clx` 等）与缺帧宽 CEL（`needs_width`）。
