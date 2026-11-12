# 预编译包（Release Package）实现规划

> 目标：让不熟悉 C++ 构建的 Hellfire / Diablo 玩家与 MOD 作者，下载一个 zip、
> 填入自己的正版 MPQ 路径、双击 `start.bat` 就能在浏览器里查看精灵 / 关卡 / 界面。
> **本包绝不打包、分发任何游戏资源（MPQ / 调色板 / 精灵 / 音频等）。**

---

## 现状评估（基于实际代码）

- 后端 `mcp-server` 已是纯 C/C++ 原生程序，`CMakeLists.txt:41` 已加
  `-static-libgcc -static-libstdc++`，StormLib 以静态 `.a` 链接
  （`build-stormlib.sh` 用 `STORM_USE_BUNDLED_LIBRARIES=ON`），理论上为单文件、零额外 DLL。
- `src/mcp/transport_ws.cpp:146` 的 `serveHttp` **已能处理同一端口的 HTTP GET**，
  但白名单只允许 `/scenes/` 前缀（行 142 注释）。扩展到托管 `web/` 即可让一个 exe
  同时提供「网页 + WS 桥」，彻底去掉对 `python -m http.server` 的依赖。
- 前端各模式的 MPQ 来源已较完整：
  - catalog / character：`sprite.list` 返回的 `archive_path` 自动定位；
  - scene / gui / monster / combat：各有输入框（`#archive` / `#sceneArchive` /
    `#guiArchive` / `#monsterArchive` / `#combatArchive`）；
  - `web/src/level.js:82` 的 `archiveStack()` 已内置 Hellfire 档案栈
    （`DIABDAT.MPQ` / `hellfire.mpq` / `hfmonk.mpq` / `hfopt1.mpq` / `hfopt2.mpq` /
    `devilutionx.mpq`），后端按序搜索。
- 痛点不在「找不到文件」，而在「每次手动填路径 + 必须另开 Python 静态服务器」。

---

## P0 让单个 exe 自包含（去掉 Python 依赖）—— 最大杠杆 ✅ 已实现

- **P0-1** 扩展 `src/mcp/transport_ws.cpp` 的 `serveHttp`：支持从一个可配置 web 根
  （默认 `web/`，相对 exe 或新增 `--web-root` 参数）取文件；保留现有 `/scenes/`
  及 artifact 目录能力。
- **P0-2** 加 MIME 表，重点 `.js`/`.mjs` → `text/javascript`（ES Module 必需，
  否则前端加载失败）、`.css`/`.html`/`.png`/`.json`/`.svg`。
- **P0-3** 目录穿越防护：规范化路径、禁止 `..`、限制在 web 根内（安全必做）。
- **P0-4** 默认路由 `/` → `index.html`；WS Upgrade 逻辑不变（同端口共存 HTTP 与 WS）。
- **P0-5** 启动参数：确认 `--ws 18765` 默认即开 HTTP 静态服务；`--stdio` 单独用时不开端口。

> **验收**：双击 exe 后浏览器开 `http://localhost:18765/` 即出现查看器，且 WS 自动连上，无需任何 Python。
> **状态（2026-11-12）**：已实现并验证——`runWs` 新增 `webRoot` 参数（默认 exe 同级 `web/`），`serveHttp` 托管 `web/` 并提供 `.html/.js/.css/.png/.json` 等 MIME，含目录穿越防护（`--path-as-is` 下的 `..` 与 `%2e%2e` 均返回 403）；`/scenes/` 原有映射保留；三套测试全绿。

---

## P1 统一 MPQ 入口（用户指向自己的正版文件）

- **P1-1** 在 `web/index.html` 顶部 `connected` 状态右侧加一个**全局「mpq配置」按钮**
  （未配置红色 / 已配置绿色），点击弹出 `#mpqModal` 对话框配置「MPQ 目录 / 主档案」，
  所有 5 种模式共用；各模式现有独立输入框作为「覆盖项」保留。
- **P1-2** 该目录作为档案栈基准：让 `web/src/level.js:82` 的 `archiveStack()`
  基于该目录拼全路径（当前应相对主档案目录解析，需确认）。
- **P1-3** 路径持久化到 `localStorage`，刷新不丢。
- **P1-4** 配置对话框**再次醒目展示**「必须自备官方正版 Hellfire 文件，本项目不提供任何资源」声明（与 README 口径一致）。

> **验收**：填一次目录，5 种模式都能正确打开自有 MPQ 里的精灵 / 场景 / 界面。

> **状态（2026-11-13 更新）**：P1-1 / P1-3 / P1-4 已实现，P1-2 部分落地（目录作为统一
> 入口并持久化，且 `level.js` 的 `mpqDir()` 已复用同一 `localStorage` 键作为默认目录）。
> 核心行为：**首次打开前端、MPQ 尚未配置时，顶部「mpq配置」按钮为红色，且前端绝不
> 自动读取 MPQ**；点击按钮在弹窗中填入自己的正版 MPQ 目录并「保存」后按钮转为绿色，
> 该目录持久化到 `localStorage`，下次进入前端会**自动连接并加载精灵目录**（即自动读取
> MPQ 获取目录）；保存当时也会立即自动连接加载，无需刷新。
> 实现要点：`web/index.html` 顶部 `.conn` 内 `#mpqConfigBtn`（`bad`/`ok` 类切红/绿背景）
> + 原生 `<dialog id="mpqModal">`（状态徽标 + 目录输入框 + 保存/取消 + 资源合规声明）；
> `web/src/main.js` 的 `getMpqDir/saveMpqDir/reflectMpqConfig/wireMpqConfig/initMpqEntry`
> （`MPQ_DIR_KEY="dhfc.mpqDir"`）在启动时 `reflectMpqConfig()` + `initMpqEntry()`；
> `web/src/level.js` 的 `mpqDir()` 复用同一键作默认目录。后端 `sprite.list` 接受
> `mpq_dir` 参数：把预生成 `recovered.json` 里的档案路径重定向到 `<mpq_dir>/<name>`，
> 从而真正读取用户自备的正版 MPQ；`sprite.decode` 的 `archives` 全栈同理。

---

## P2 一键构建 + 打包（Windows）

- **P2-1** 新增 `build-release.ps1`（或适配 Git Bash 的 `build-release.sh`）：自动
  `git submodule update --init` → `build-stormlib.sh` → cmake configure/build
  `mcp-server`（Release, MinGW Makefiles）→ 产出 `mcp-server.exe`。
- **P2-2** 把 `build-stormlib.sh:21` 硬编码的 `MINGW_BIN` 默认值改为可配置 / 自动探测
  （避免写死 `/d/develop/mingw64/bin`）。
- **P2-3** 新增 `package.ps1`：组装 `DiabloHFConverter-win-x64.zip`，含
  `mcp-server.exe` + `web/` + `start.bat` + `README`（含免责声明）+ 资源声明文件。
  **绝不打进任何 MPQ / 资源**。
- **P2-4** `start.bat`：启动 `mcp-server.exe --ws 18765`，打印并自动打开
  `http://localhost:18765/`。

> **验收**：在干净机器上跑 `build-release.ps1` + `package.ps1` 得到可分发包。

---

## P3 自包含校验（必须过）

- **P3-1** 在**无 MinGW / 无 Python** 的干净 Windows（或 VM）解压 zip，用
  `dumpbin /dependents mcp-server.exe` 确认只依赖系统 DLL
  （KERNEL32 / USER32 / WS2_32 / WININET …）。`CMakeLists.txt:41` 已加
  `-static-libgcc -static-libstdc++`，StormLib 静态 `.a` 且 bundled，理论上零额外 DLL——**需实测确认**。
- **P3-2** 用一份自有正版 MPQ 走通：开浏览器 → 选精灵 / 场景 / 界面 → 正常渲染。
- **P3-3** 在 README 新增「预编译包」章节：下载解压 → 填入自己的 MPQ 目录 →
  双击 `start.bat` → 开浏览器。

---

## P4 发布与版本（引流前置，可选进阶）

- **P4-1** `git tag` 打版本（如 `v0.1`），在 Gitee / GitHub Releases 上传 zip +
  release notes（**再次注明不提供资源**）。
- **P4-2**（可选）Gitee CI / GitHub Actions 自动出 Windows artifact；MPQ 相关集成测试
  因版权不进 CI，仅跑非依赖 MPQ 的单元 / 核心测试。
- **P4-3**（可选）随 release 发一段用自己的正版资源渲染的演示 GIF / 短视频，用于引流。

---

## 功能方向 A：标准格式导入/导出 + AI 高清化（见 docs/UPSCALE_PLAN.md）

- 方向 A（标准图片/文本格式的导出，以及导入回 HF 支持的 MPQ）与 AI 补值高清化已单列规划：
  **[docs/UPSCALE_PLAN.md](UPSCALE_PLAN.md)**。
- 该方向复用本预编译包的「单 exe 自包含」成果（P0）：用户解压即用，在浏览器里查看、导出、
  放大自己的正版素材。
- 合规护栏（零权重、产物本地、仅超分）见 **[docs/COMPLIANCE.md](COMPLIANCE.md)**，实现 A 时同样适用。

---

## 优先级

P0 → P1 → P2 → P3 是「能发预编译包」的最小闭环，P4 为发布锦上添花。
**P0 性价比最高**——不动前端、不碰资源合规，单 exe 直接去掉最大使用门槛。
功能方向 A（见 docs/UPSCALE_PLAN.md）与本打包闭环**可并行**：其 P-U0 / P-U1 零风险、
且能产出引流用的「前后对比」demo 物料，建议尽早做。
