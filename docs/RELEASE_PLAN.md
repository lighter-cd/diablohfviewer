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

- **P2-1** ~~新增 `build-release.ps1`~~ → **已由 `scripts/release.py` 覆盖**（2026-11-16）：
  脚本负责打包/打 tag/ 发布，构建仍沿用 README 的 `build-stormlib.sh` + `cmake` 两步，避免在脚本里
  重复构建逻辑（构建需先停掉正在运行的 `mcp-server.exe`，见 `AGENTS.md`）。
- **P2-2** ~~把 `build-stormlib.sh:21` 硬编码的 `MINGW_BIN` 默认值改为可配置 / 自动探测~~
  —— 打包侧已支持 `--dll-dir` / `MINGW_BIN` 环境变量与 PATH 自动探测，构建脚本仍保留原默认值。
- **P2-3** **已实现**：`scripts/release.py` 组装
  `dist/DiabloHFViewer-<version>-win64.zip`，含 `mcp-server.exe` + MinGW 运行时 DLL（按 PE
  导入表自动识别，当前只有 `libwinpthread-1.dll`）+ `web/` + `start.bat` + `LICENSE` +
  `README.md` + `VERSION.txt` + 可选 `mpq-info/dict/recovered.json`（`--no-dict` 关闭）。
  **绝不打进任何 MPQ / 资源**：打包前扫描文件表，出现 `*.mpq` 直接报错。
- **P2-4** **已实现**：`start.bat` 由脚本生成，内含 `cd /d "%~dp0"`、
  `mcp-server.exe --ws 18765`，并在启动 2 秒后用 `explorer` 打开
  `http://localhost:18765/`。

> **验收**：在干净机器上跑 `build-release.ps1` + `package.ps1` 得到可分发包。

---

## P3 自包含校验（必须过）

- **P3-1** 在**无 MinGW / 无 Python** 的干净 Windows（或 VM）解压 zip，用
  `dumpbin /dependents mcp-server.exe` 确认只依赖系统 DLL
  （KERNEL32 / USER32 / WS2_32 / WININET …）。`CMakeLists.txt:41` 已加
  `-static-libgcc -static-libstdc++`，StormLib 静态 `.a` 且 bundled，理论上零额外 DLL——**需实测确认**。

  > **实测（2026-11-16）**：`objdump -p build-mcp/mcp-server.exe` 的导入表为
  > `KERNEL32 / USER32 / WS2_32 / WININET / api-ms-win-crt-*`（UCRT，Windows 10+ 自带）
  > 加上**一个** MinGW 运行时 `libwinpthread-1.dll`。因此发布包必须随包带上该 DLL
  > （`scripts/release.py` 自动从 MinGW `bin/` 取），其余无需打包。
- **P3-2** 用一份自有正版 MPQ 走通：开浏览器 → 选精灵 / 场景 / 界面 → 正常渲染。
- **P3-3** 在 README 新增「预编译包」章节：下载解压 → 填入自己的 MPQ 目录 →
  双击 `start.bat` → 开浏览器。

---

## P4 发布与版本（引流前置，可选进阶）

- **P4-1** **已实现（2026-11-16）**：`scripts/release.py --version v0.1.0`
  自动打 annotated tag（`vMAJOR.MINOR.PATCH`）、把 tag 推到 Gitee 与 GitHub 两个远端
  （按 URL 域名识别，不依赖远端名），并分别调用 GitHub REST（`/releases` +
  `uploads.github.com/.../assets`）与 Gitee v5 API（`/releases` + `/releases/{id}/attach_files`）
  创建发行版、上传 `zip` 与 `.sha256`。release notes 默认取「上一 tag → HEAD」提交列表，
  末尾自动附「不提供游戏资源」声明；同名资产重跑会先删后传，幂等。
  依赖环境变量 `GITHUB_TOKEN`（repo / Contents:write）与 `GITEE_TOKEN`（projects），
  支持 `--dry-run` / `--skip-github` / `--skip-gitee` / `--no-package`。
  测试：`tests/release_pack_test.py`（dry-run 打包、结构/校验和/`--no-dict`/禁 MPQ 断言）。
- **P4-2**（可选）Gitee CI / GitHub Actions 自动出 Windows artifact；MPQ 相关集成测试
  因版权不进 CI，仅跑非依赖 MPQ 的单元 / 核心测试。
- **P4-3**（可选）随 release 发一段用自己的正版资源渲染的演示 GIF / 短视频，用于引流。

---

## 发布脚本实现（2026-11-16，已完成）

`scripts/release.py`（纯 Python 标准库，Windows / Linux 通用，不依赖 `gh`）。

```bash
python scripts/release.py --version v0.1.0 --dry-run      # 只打包 + 校验和
$env:GITHUB_TOKEN="ghp_xxx"; $env:GITEE_TOKEN="xxx"       # 正式发布所需
python scripts/release.py --version v0.1.0                # tag + 双平台 release
```

| 参数 | 作用 |
|------|------|
| `--version vX.Y.Z` | 必填；同时作为 tag、release 名与 zip 文件名 |
| `--dry-run` | 只打包 + 写 `.sha256`，不建 tag、不 push、不调 API |
| `--notes-file` | 覆盖自动生成的 release notes（默认取上一 tag→HEAD 提交列表） |
| `--out-dir` | 产物目录，默认 `dist/`（已 gitignore） |
| `--exe` / `--dll-dir` | 指定 exe 与 MinGW `bin/`（默认 `build-mcp/mcp-server.exe`、`MINGW_BIN`、PATH 自动探测） |
| `--no-dict` | 不打包 `mpq-info/dict/recovered.json` |
| `--no-package` | 复用 `dist/` 里已有的 zip（只发布） |
| `--no-push` | 建 tag 但不推远端 |
| `--skip-github` / `--skip-gitee` | 只发单边 |
| `--allow-dirty` | 允许工作区有未提交改动（常规发布**不应**使用） |

实现要点：

1. **远端解析**：遍历 `git remote` 取 URL，按域名（`github.com` / `gitee.com`）定位两端并解析
   `owner/repo`；**不依赖远端名**，`origin` 指 Gitee 还是 GitHub 都不影响；
2. **DLL 依赖**：自写 PE 导入表解析（`pe_imports`），只把真正 import 的 MinGW 运行时
   打进包（当前为 `libwinpthread-1.dll`），不靠字符串扫描（避免把 libgcc 里的提示串误判）；
3. **合规**：打包前检查文件表，出现 `*.mpq` 立即报错；notes 末尾统一附免责声明；
4. **幂等**：GitHub 先 `GET /releases/tags/{tag}` 复用，同名资产 422 时删除重传；
   Gitee 建 release 失败则按 tag 查询复用；附件直接重传；
5. **chunked 上传**：资产以整文件字节 POST（本包约 2 MB，远低于平台限制）。

> 平台限制：GitHub 单个资产 ≤ 2 GB；Gitee 发行版附件 ≤ 100 MB。两平台均支持网页手动
> 上传作为脚本的替代方案（`git tag` + push，再在 Releases 页面新建并上传 `dist/` 内容）。

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
