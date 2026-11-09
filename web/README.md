# Sprite Viewer（web 前端骨架）

浏览器里的 Diablo 1 / Hellfire 查看器。本前端**不读 MPQ、不碰文件系统**，
只通过 WebSocket 调用本地 `mcp-server` 暴露的 `sprite.*` / `palette.*` / `scene.*` 工具，
拿到 RGBA 帧（Canvas）或把场景 PNG 预览绘制到 Canvas。

> 阶段 2/3/5 的浏览器查看器，共 **5 种浏览模式**：目录浏览、角色查看器、
> 怪物 / NPC 查看器、技能 / 战斗查看器、**场景查看器**。
> 技能特效（`lm`/`fm`/`qm`）作为可浏览动画纳入；TRN 上色已实装为开关。
> 场景查看器调用 `scene.list` / `scene.build` 复原**完整关卡场景**（Tristram 城镇 +
> L1–L4 预设），支持缩放 / 平移 / 叠加层 / dLight 光照，见下方独立章节。

## 运行

```bash
# 1) 启动后端 WS 桥（默认端口 18765，见 src/main.cpp）
mcp-server.exe --ws 18765
#    或同时开 stdio（开发方便）： mcp-server.exe --stdio --ws 18765

# 2) 用任意静态服务器托管本目录（ES module 无法在 file:// 下加载）
cd web
python -m http.server 8000        # 或： npx serve .

# 3) 浏览器打开 http://localhost:8000/
```

打开后点 **Connect**（默认 `ws://localhost:18765`），顶部「浏览模式」可在 5 种视图间切换：

- **目录浏览**（默认）：左侧目录树由 `sprite.list` 构建；点任一精灵即调用 `sprite.decode`
  并绘制，用下方「方向 / 帧」滑块翻看，或点「▶ 播放」循环播放当前方向的动画。
- **角色查看器**：见下方独立章节。
- **怪物 / NPC 查看器**：见下方独立章节。怪物/NPC 没有 MPQ listfile，目录由后端按
  DevilutionX 数据表（`monstdat.tsv` / `towners.tsv`）**合成**（`sprite.list(family:"monsters"/"towners")`）。
- **技能 / 战斗查看器**：四阶段时间线（施法 → 投射 → 命中 → debuff）。
- **场景查看器**：见下方独立章节，复原完整关卡地图。

目录树支持两种分组方式，可在左侧顶部「分组」处切换：

- **按类型合并**（默认）：`family → class → type → 精灵`。
- **按 MPQ 包**：先按精灵所属 MPQ（如 `DIABDAT.MPQ` / `hellfire.mpq`），再按档案内目录
  路径逐层展开（`missiles → fireplar.cl2`），便于按资源来源浏览。

两种模式下所有分支节点都**默认收起**，点击节点标题即可展开/收起；顶部另有
「展开全部 / 收起全部」一键展开或折叠整棵树。

### 8 方向播放（通用）

玩家精灵在文件中是**方向优先**布局（8 方向 × N 帧），前端按 `总帧数 / 方向数` 切分：

- **方向数**：默认 plrgfx=8，其它=1；可选 1 / 2 / 4 / 8。
- **方向**滑块：选择当前显示的方向（0–7）。
- **帧**滑块：在当前方向内翻帧（0 – 每方向帧数-1）。
- **▶ 播放 / ⏸**：以「速度」框指定的 fps（默认 12）循环播放当前方向的帧。
- 状态栏始终显示**当前帧**尺寸（如 `帧 96×128`），以及 `dir / 帧内` 位置。

### 角色查看器（浏览模式）

切到「角色查看器」后，左侧出现角色面板，由 `sprite.list(family:"plrgfx")` 一次性构建索引，
仅列出 MPQ 中**真实存在**的组合：

1. **职业**：warrior / rogue / sorcerer / monk（及 Hellfire 的 barbarian / bard，按实际出现填充）。
2. **动作**：Stand / Walk / Attack / Hit / Lightning / Fire / Magic / Death / Block（友好名后附
   type 代码，如 `Attack (at)`），仅显示该职业真实存在的动作。
3. **盔甲**：Light / Medium / Heavy（仅该职业当前动作存在的）。
4. **武器**：None / Unarmed / Sword / Dagger / Bow / Axe / Mace / H2H（仅存在的）。

切换任意一项即按公式 `plrgfx\{class}\{c}{a}{w}\{c}{a}{w}{type}.cl2` 拼路径并解码，
再用上方 8 方向控件播放。装备（武器/盔甲）**不做 alpha 合成**——它们本就是独立精灵文件，
「组合浏览」等价于枚举不同文件名（符合 D1 把武器/盔甲 bake 进文件名前缀的设计）。但**特效叠加层**
支持 alpha 合成（见下方「特效叠加层（alpha 合成）」）。

### 怪物 / NPC 查看器（浏览模式）

怪物（`monsters\`）与 NPC（`towners\`）精灵**没有 MPQ listfile**，后端无法靠哈希恢复其
真实名，因此「怪物 / NPC 查看器」模式下的目录由后端从 DevilutionX 数据表**合成**：

1. **类型**：怪物（Monster）/ NPC（Towner）二选一。
2. **MPQ**：选解码所用的 MPQ 包（取自目录归档的 `archive_path`，怪物/NPC 真实存在于
   `DIABDAT.MPQ` 等；合成条目本身不携带 `archive_path`）。
3. **怪物**（选「怪物」时）：先选怪物（按 `monstdat.tsv` 的 `assetsSuffix` 分组，如
   `zombie\zombie`），再选**动作**——按动画字母 `n/w/a/h/d/s` 枚举，友好名为
   Stand / Walk / Attack / Hit / Death / Special（Special 仅当 `frames[6]` 末项 > 0 时存在），
   下拉项附 `帧/方向` 数。切换即 `sprite.decode(name,width:"auto")` 并播放。
4. **NPC**（选「NPC」时）：列出 `towners.tsv` 的 `animPath`（如铁匠 Griswold、医师 Pepin…）
   及硬编码的 `towners\animals\cow.cel`，均为单方向 `.cel`，解码后逐帧/播放。

怪物方向数不再手填：后端 `sprite.decode` 按 `monstdat` 的 `frames[6]` 切分并返回
**`directions`** 字段（如僵尸 Stand：88 帧 ÷ 11 帧/方向 = 8 方向），前端据此自动切分；
NPC 固定单方向。两者宽度均由后端自动查表（`monstdat.tsv` / `towners.tsv`）。

**跨 MPQ 搜索**：怪物/NPC 精灵分散在多个 MPQ（基础怪物在 `DIABDAT.MPQ`，Hellfire 怪物在
`hellfire.mpq`，个别在 `hfopt1.mpq`…）。解码时前端把目录里收集到的**全部 MPQ** 作为
`archives` 传给 `sprite.decode`，后端逐个档案查找精灵/调色板，因此上方「MPQ」下拉选哪个都能
正确解码（含所有 Hellfire 怪物）。MPQ 路径统一规范为 `/`，避免 `recovered.json` 里 `/` 与 `\`
混用导致同一档案被重复打开而报 `cannot open archive`。

### 特效叠加层（alpha 合成）

画布下方有一块「特效叠加层」面板（默认折叠，点「展开」）。在**任一浏览模式**选中基础精灵后，
可把任意特效精灵作为独立图层叠加到底图当前帧上，按 `(dir, frame)` 对齐并做源 alpha 合成：

- **快速加**：面板自动按当前模式给出常用特效——
  - 角色模式：当前职业的技能特效 `plrgfx\{c}\{caw}\{caw}{lm/fm/qm}.cl2`（闪电 / 火 / 魔法）；
  - 怪物模式：该怪物的 Special `s` 动画（`..._{nwahd}.cl2` → `..._s.cl2`）。
- **通用加**：下拉列出当前目录全部精灵，可任意选取做底图叠加（如把一件物品/技能特效叠到角色上）。
- 每个叠加层可单独**启用/停用**、设 `x/y` 偏移、**移除**；元信息行显示「· 叠加层 N」。

实现见 `web/src/viewer.js` 的 `drawComposite` 与 `web/src/main.js` 的 `overlayLayers` 状态
（叠加层复用基础精灵的调色板 / TRN 上下文，由 `buildDecodeArgs` 统一构造解码参数）。

### 技能 / 战斗查看器（浏览模式）

第 4 种浏览模式，以**施放者为中心**同屏显示 施放者 + 目标 + 攻击/施法效果：

1. **来源**：三选一——玩家技能（51 个，来自 `spelldat.tsv`）、怪物远程攻击（25 条：16 类
   `GetMissileType` AI，含唯一怪带来的 `AcidUnique`/`LazarusSuccubus`；加 9 个「AI 函数内直接
   `AddMissile`」的形态——Counselor 按智力分流的 4 种弹与近身 Flash、Familiar 闪电、Gloom 冲撞、
   Mega 地狱火、Hork Demon 召唤）、怪物特殊能力（3 个：受击瞬移、死亡酸池、潜行隐身），或
   陷阱法术（10 个：墙陷阱箭矢/火焰弹/闪电、宝箱箭矢/火焰箭/新星/火焰之环/偷药水/吸法力、
   地狱火喷火口）。
2. **技能 / 攻击**：选具体条目。**下拉项与顶部标题优先显示后端 `name_zh`（中文名，如
   `火栓 (Firebolt)`），确无翻译时回退英文名**；选中后，画布下方的**信息区**会列出
   **英文名**（`name`）、**中文名**（`name_zh`，确有翻译时）、**法术等级**（`level`，来自
   `spelldat.bookLevel`）、**各级法力**（`mana_by_level`，按 `GetManaAmount` 公式算出的
   1..bookLevel 基础法力曲线）、**各级视觉参数**（`visual_by_level`，表格列出每级的持续时间
   `duration_ticks`/`duration_sec`、弹速 `speed`、范围 `range_tiles`/`radius_tiles`、弹射物数
   `count` 及法术特有字段如 `segments`/`wall_length_tiles`；含角色/地下城层依赖项标「注」；
   无级别缩放的单级/实用法术显示「无级别视觉缩放」）、**攻击形态**（`variant`，仅多形态怪物攻击）、
   **触发条件**（`condition` / `trigger`，中文说明该形态或能力何时使用）、**源码位置**
   （`source_ref`，仅特殊能力）与**合成中文描述**（`description`，含
   元素系、法力消耗、最低智力、法术等级与每级法力变化、投射物 / 命中 / 状态阶段），便于核对。
3. **玩家职业 / 怪物 MPQ / 怪物 / 怪物动作**：决定同屏两个角色。
4. **方向**（控制条「方向」滑块，0–7）：以施放者为中心旋转整场——目标落在施放者朝向的
   方位并**回身面向施放者**，两者位置与朝向同步重算。切到「怪物远程攻击」时施放者与目标
   互换（怪物施放、玩家成为目标），布局与朝向随之变化。此模式下「方向数」选择器禁用。
5. **阶段**：`◀ / ▶` 单独查看 施法 → 投射 → 命中 → debuff；底部「▶ 播放」在
   `自动连播全阶段` 勾选时按时间线一次走完，否则只循环当前阶段。

**方向不变量**：解码结果按方向拆成 `FrameSet.groups[]`，播放只在当前方向的那组帧里推进
（`frameSetAt` 把帧号对组长度取模），因此**每个阶段在当前方向内播完、播放全程不切换方向**。
时间线的帧预算取「各方向帧数的最大值」，与方向无关，故旋转方向时无需重建时间线。
投射物从施放者飞向目标，命中 / debuff 落在目标上。

数据来自后端 `spell.list`（`spelldat` / `misdat` / `missile_sprites` / `monstdat` 四表 join）。注意：

- `plrgfx` 的 `lm/fm/qm` 是**角色施法动作**（cast 阶段），不是火球之类攻击效果；
- 火球 / 火墙等**攻击效果**是 `missiles\{name}{N}.cl2` 编号文件，不在 `sprite.list` 的目录中，
  由 `spell.list` 合成清单并提供**显式 `width`**（编号文件不能用 `width:"auto"`）；
  这些编号文件其实是**各方向的动画**（DevilutionX `animFAmt` / `spritesForDirection(Direction16)`），
  播放时只取当前方向对应的那一份，不再把 16 个文件平铺成一条会跨越方向的动画；
- debuff 阶段用 TRN（`stone.trn` / `infra.trn`）对**目标**层做**近似**着色，或叠加状态精灵
  （`manashld` / `shatter1` 等）；
- 命中 / 爆炸阶段在数据表中无外键，按命名约定自动关联，其余进 `impact_catalog` 候选
  （标 `confidence:"catalog"`）。

## 目录结构（纯静态，无 Node 运行服务）

```
web/
├── index.html        # 入口：连接栏 + 目录树 + 当前资源名 + 错误条 + Canvas + 控制条
├── styles.css        # 极简暗色样式
├── src/
│   ├── client.js     # McpClient：WS + JSON-RPC（id 配对，并发安全）
│   ├── catalog.js    # sprite.list 结果 → 嵌套树 → 可点击 DOM 树
│   ├── viewer.js     # rgba_base64 → ImageData → putImageData（最近邻放大）
│   └── main.js       # 编排：connect → sprite.list(+kind:palette 填下拉框) → 点击 → sprite.decode → 绘制
└── README.md
```

## 与后端的契约

- 传输：标准 RFC6455 WebSocket；每帧一个 JSON-RPC 2.0 请求，无需 `initialize`。
- 工具调用：`{method:"tools/call", params:{name, arguments}}`，回包按 `id` 配对。
- `sprite.decode` 返回 `frames:[{index,width,height,rgba_base64}]`，
  `rgba_base64` 为**原始 RGBA8**（`w*h*4` 字节），不是 PNG data URL。
- `width:"auto"` 由后端按精灵路径自动查 DevilutionX 数据表取帧宽。
- `sprite.list` 支持 `kind`：`sprite`（默认，CL2/CEL）/ `palette`（仅 `.pal`）/ `all`。
  前端用 `kind:"palette"` 拉取**全部 MPQ** 内的 `.pal`（每条带 `archive_path`）填入下拉框。
- `sprite.decode` 额外接受 `archives`（额外 MPQ 路径数组）：精灵与其调色板常不在同一档案
  （如 DIABDAT 的 `missiles\fireplar.cl2` 配 hellfire 的 `nlevels\cutl5.pal`）。前端把
  「精灵所在 MPQ + 调色板所在 MPQ」一并传入，`sprite.decode` 在档案栈中查找；若只传 `archive`
  而调色板在别的档案，会报 `extract failed (name not found?)`。

## 交互细节

- **当前资源路径**：点选精灵后，主视窗顶部醒目显示其完整 in-archive 路径（`currentName`）。
- **调色板下拉框**：连接后由 `sprite.list(kind:"palette")` 填充全部 MPQ 的 `.pal`；切换即对该
  精灵重新解码（无需手填路径）。`main.js` 按选中调色板的 `archive_path` 连同精灵的 `archive`
  一并经 `sprite.decode` 的 `archives` 传入，因此**即使调色板在另一个 MPQ 也能正确上色**。
  不同家族精灵适用不同调色板，可手动切换观察。
- **错误显式提示**：遇到暂不支持的格式、缺 `archive_path`、或解码失败，主视窗会以红色错误条
  （`errorBanner`）明确提示原因，而不会静默无反应。

## 已知限制（后续打磨）

- `sprite.list` 一并返回 `archive_path`（取自 recovered.json 的档案 `path`），`main.js`
  直接用它对 `sprite.decode` 解码——点选精灵即可直接解码，无需手填路径。
  （缺失时错误条会提示，便于排查 recovered.json 数据问题。）
- 调色板下拉框在**角色查看器 / 怪物查看器模式**下已预筛选为「关卡调色板」（`levels\*`/`nlevels\*`），
  目录浏览模式下列出全部 `.pal`；这是保真折中——玩家/怪物精灵只有在关卡调色板下颜色才正确。
- **TRN 上色已实现（开关切换）**：控制条新增「应用 TRN」复选框 + TRN 预设下拉
  （职业默认 / infra 红外视觉 / stone 石化），默认关闭（原调色板模式）。开启后 `sprite.decode`
  传 `apply_trn:true`，后端取 256 字节 TRN 重映射调色板后解码；meta 行显示 `TRN: <name>`。
  **实测关键结论**：原版 MPQ 内 `plrgfx\trnwarrior/trnrogue/trnsorcerer/trnmonk.trn` 均不存在，
  故「职业默认」路径是**忠实的 no-op**（仍用调色板上色，与游戏内一致）；`infra.trn`（红染）/
  `stone.trn`（灰染）可经预设显式施加看效果。每精灵 TRN `<spritePath>.trn` 在 MPQ 内命中为 0，无实际效果。
- 8 方向切分按 `总帧数 / 方向数` 整除近似；若某精灵并非严格方向优先（如部分非玩家精灵），
  可手动把「方向数」调为 1 退回逐帧浏览。

## 场景查看器（第 5 种浏览模式）

切换到「场景查看器」后，共享 Canvas 被隐藏，改为一个可平移 / 缩放的视口，里面是两张
绝对定位、像素级对齐的 `<canvas>`：场景底图 + SOL 叠加层。

- **加载**：进入该模式会自动调用 `scene.list` 填充「场景」下拉；点「加载场景」调用
  `scene.build {id, archive, archives, preview_max_width:1000}`。默认使用目录
  `sprite.list` 收集到的 MPQ 档案栈（`allArchives`），也可在「MPQ 路径」输入框里
  手动覆盖主档案。
- **数据契约**：后端返回 `image_base64` / `overlay_base64`（均为 PNG 字节的 base64），
  前端把 base64 解码为 Blob → `createImageBitmap` → `drawImage` 到两张 canvas。
  不创建 `<img src>`、不拼 `data:` URL。两张图由同一
  `SceneProjection` 生成、按同一整数 `scale` 降采样，尺寸完全一致。PNG 的 IDAT 由后端
  `src/decoders/deflate.cpp`（fixed-Huffman + 贪心 LZ77）压缩，`scene.build(tristram)`
  预览约 343 KB base64，远低于 Firefox `data:` URL ~2 MB 上限（早期未压缩 stored 块
  ≈2.2 MB 会显示「无法显示的图片/空图」），可被标准 zlib 无损解压。
- **缩放 / 平移**：滑块（1–8×）或滚轮缩放，视口内按下拖拽平移；「复位视图」回到
  全图适配。`applySceneTransform()` 对两张 canvas 施加同一组 `width/height/left/top`，
  因此叠加层永远与底图对齐。`image-rendering:pixelated` 保证放大后仍是清晰像素。
- **叠加层**：半透明等距网格线 + 每格 SOL 标志着色（红=Solid、蓝=透明、
  黄=BlockLight），由后端按网格数组生成；开关控制显隐。
- **光照（dLight）**：勾选后 `loadScene()` 传 `scene.build(lighting:true)`，后端按
  场景环境光（城镇 0、地牢 15）+ 中心 radius-15 火把计算每格光照，用 DevilutionX
  `LightTables` 烘焙调色板暗部；切换复选框会重建场景。
- **提亮**：Tristram 夜景均值亮度仅 13、67% 像素 <24，不提亮观感近似空图。
  「提亮（夜景）」**默认勾选**，`loadScene()` 每次按开关状态给 `#sceneLayers` 施加
  `.bright` 类（`web/src/main.js`），CSS `web/styles.css` 用
  `brightness(1.7) contrast(0.85) saturate(1.1)` 抬暗部、菱形背景仍近黑，
  不改变后端输出。

场景由后端 `scene.build` 用 DevilutionX 两遍绘制（地板菱形 + 墙体）复原，输出
**不透明背景、无透明空洞**（`transparent_pixel_count == 0`）。场景目录：Tristram
城镇（4 个 sector DUN + 预填微瓦片）+ L1/L2/L3/L4 预设（`l1_rnd2`、`l1_skngdo`、
`l2_blind`、`l3_anvil`、`l4_diab1`）。`scene.path` 可在同一 SOL 网格上做 A* 寻路并返回
与预览对齐的路径坐标（供后续前端叠加）。原理与资产路径见
`docs/ARCHITECTURE.md` §9.3 与 `docs/PROJECT.md`。

## 关卡 Tile 查看器（level.html）

`http://localhost:8000/level.html`（同样需要 `mcp-server --ws 18765`）。页面调用
`level.inspect`，默认预填 Tristram `levels/towndata/town.{cel,til,min,sol,pal}`、
`DIABDAT.MPQ` 路径与 `megatile=71`（水池）。连接后会自动检查一次，并可用 `◀ / ▶`
浏览上一个/下一个 megatile；「图块集」下拉可切换 Tristram / L1–L4 / Hellfire L5–L6，
自动填入对应路径、blocks/micro_len 与主 MPQ。可看到：

- 4 个 micro piece（257–260）及每个 `mt` 的 `TileType / frame / data`；
- 引用的全部 CEL 帧（594–605）独立 PNG，点击切换 1×/4×；
- 这些 micro 拼出的等距透明底预览 `assembly`（点击切换 1×/4×）。

这是区分 **tiles 错** 还是 **objects 错** 的关键工具。2026-11-09 的 tile 纵向翻转修复
（triangle/trapezoid 被二次翻转，导致水池不圆）就是用它定位的。
