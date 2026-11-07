# Sprite Viewer（web 前端骨架）

浏览器里的 Diablo 1 / Hellfire 精灵查看器。本前端**不读 MPQ、不碰文件系统**，
只通过 WebSocket 调用本地 `mcp-server` 暴露的 `sprite.*` / `palette.*` 工具，
拿到 RGBA 帧后用 Canvas 绘制。

> 这是阶段 2 的第一版骨架：先做「选精灵 → 看单帧」。8 方向播放、装备组合浏览、
> 技能特效叠加、TRN 上色等增强在 `docs/ARCHITECTURE.md` §10.4 的后续步骤里。

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

打开后点 **Connect**（默认 `ws://localhost:18765`），左侧目录树由 `sprite.list`
按 `family → class → type` 构建；点任一精灵即调用 `sprite.decode` 并绘制第 0 帧，
用下方 Frame 滑块翻看其余帧。

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
  前端用 `kind:"palette"` 拉取档案内全部调色板填入下拉框。

## 交互细节

- **当前资源路径**：点选精灵后，主视窗顶部醒目显示其完整 in-archive 路径（`currentName`）。
- **调色板下拉框**：连接后由 `sprite.list(kind:"palette")` 填充档案内全部 `.pal`；切换即对该
  精灵重新解码（无需手填路径）。不同家族精灵适用不同调色板，可手动切换观察。
- **错误显式提示**：遇到暂不支持的格式、缺 `archive_path`、或解码失败，主视窗会以红色错误条
  （`errorBanner`）明确提示原因，而不会静默无反应。

## 已知限制（后续打磨）

- `sprite.list` 一并返回 `archive_path`（取自 recovered.json 的档案 `path`），`main.js`
  直接用它对 `sprite.decode` 解码——点选精灵即可直接解码，无需手填路径。
  （缺失时错误条会提示，便于排查 recovered.json 数据问题。）
- 调色板下拉框列出整个档案的全部 `.pal`，未按家族预筛选；后续可随选中精灵的家族自动优选。
- 暂只做"选精灵 → 看单帧"（Frame 滑块翻帧）；8 方向播放 / 装备组合浏览 / TRN 上色待加。
