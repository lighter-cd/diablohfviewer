# Sprite Viewer（web 前端骨架）

浏览器里的 Diablo 1 / Hellfire 精灵查看器。本前端**不读 MPQ、不碰文件系统**，
只通过 WebSocket 调用本地 `mcp-server` 暴露的 `sprite.*` / `palette.*` 工具，
拿到 RGBA 帧后用 Canvas 绘制。

> 阶段 2 的浏览器查看器：支持「选精灵 → 看单帧 / 8 方向播放」，并提供独立的
> 「角色查看器」面板（职业 × 动作 × 盔甲/武器 组合浏览）。技能特效（`lm`/`fm`/`qm`）
> 作为可浏览动画纳入；TRN 上色为后续增强。

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

打开后点 **Connect**（默认 `ws://localhost:18765`），顶部「浏览模式」可在两种视图间切换：

- **目录浏览**（默认）：左侧目录树由 `sprite.list` 构建；点任一精灵即调用 `sprite.decode`
  并绘制，用下方「方向 / 帧」滑块翻看，或点「▶ 播放」循环播放当前方向的动画。
- **角色查看器**：见下方独立章节。

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
再用上方 8 方向控件播放。装备/特效**不做 alpha 合成**——它们本就是独立精灵文件，
「组合浏览」等价于枚举不同文件名（符合 D1 把武器/护甲 bake 进文件名前缀的设计）。

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
- 调色板下拉框在**角色查看器模式**下已预筛选为「关卡调色板」（`levels\*`/`nlevels\*`），
  目录浏览模式下列出全部 `.pal`；这是保真折中——玩家精灵只有在关卡调色板下颜色才正确。
- **TRN 上色未做（原理已逆向，待决定是否实施）**：当前始终用选中的 `.pal`（如
  `diablo.pal` / `levels\*`）上色。TRN 是 256 字节调色板索引重映射表（`ClxApplyTrans`），
  分两级——**每精灵 TRN** `<spritePath>.trn`（`GetPlayerGraphicTRN`，MPQ 内命中为 0，无实际效果）
  与**职业 TRN** `plrgfx/<class>.trn`（`GetClassTRN`，真正生效层）；`LoadPlrGFX` 顺序为
  先每精灵后职业。若落地，`sprite.decode` 加 `apply_trn:true` 取职业 TRN 作用到帧再返回。
- 8 方向切分按 `总帧数 / 方向数` 整除近似；若某精灵并非严格方向优先（如部分非玩家精灵），
  可手动把「方向数」调为 1 退回逐帧浏览。
