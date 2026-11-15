# AGENTS.md

## 优先参考 D:\work\xt\diablo\DevilutionX 的实现
  面对 **mcp-server 的功能需求**，**优先查看** D:\work\xt\diablo\DevilutionX 的代码实现方式，参考项目中找不到相关实现，再自行寻找解决方案。

## 改动记录与文档同步（硬规则，不可违反）

1. **文档同步**：每次新增工具/流程，或修改其用法、思路后，必须在本轮内同步更新对应文档目录 docs/ 中，以及项目根目录的 README.md 中 
   **不允许"代码已改、文档没跟上"**；新增工具时同时给出**源码路径**与**测试脚本**。
   - docs/ 中的文档面向开发者，记录更详细的信息
   - README.md 面向使用者，只记录使用者角度的内容，如 构建方法，启动测试的方法，构建产品的使用方法。
   - docs/PROJECT.md 中记载当前进度列表，完成的项目需要在其中标记为完成。
  
2. **改动历史**：每当本轮产生任何改动
   （编辑/新建/删除代码、配置、文档），在结束回复前必须把本轮总结**追加**到项目根 `changed-history.txt`。

   ```text
   ## YYYY-MM-DD HH:mm:ss
   - <一句话：改了什么、为什么>
   - 涉及: <文件1>, <文件2>
   ```

   - 格式 UTF-8；**一轮任务一条**，多文件改动合并为一条。
   - 追加写入（append），**严禁覆盖**已有内容；文件不存在则先创建。
   - 时间精确到秒并取**改动发生当时**的真实系统时间（必要时先 `Get-Date` 确认，**禁止凭记忆编造**），
     改动完成后立即追加；该记录作为后续 git commit 的时间与内容依据，时间务必准确。
   - 总结用中文、一行说清，不粘贴 diff。
   - `changed-history.txt` 自身的写入不重复记录；本轮只提问/只读、无任何改动则不写。

## 文件写入与版本管理（不可违反）

  每当本轮产生任何改动，在写完 `changed-history.txt` 后，将所有的改动 添加 & 提交 & **推送**，
  并附简单的内容描述。

  **推送策略（默认 Gitee，GitHub 需显式声明）**：
  - `origin` = **Gitee（主）**，日常一律 `git push`（等价 `git push origin master`），
    不要顺手推 GitHub；
  - `github` = **GitHub（镜像）**，只有用户在当轮明确要求（如「也推 GitHub」「同步镜像」）时，
    才执行 `git push github master`；
  - **例外**：版本发布（`scripts/release.py` 打 tag + 建 Release）本身是显式操作，
    脚本会按 URL 域名同时更新两端，这不违反本策略；
  - 换机器/重新克隆时保持命名：主远端 `origin` 指向 Gitee，镜像 `github` 指向 GitHub
    （脚本只认域名，但人工命令按名字写，命名统一最省心）。

## 单元测试
  - 所有源码中的各单元，包括cpp的类，c中的文件级别模块，可以被其他类和模块引用的内容，都必须配合完整的测试用例。
  - 每轮源码 创建、改动、删除后，都必须完整运行所有的测试用例。

## 构建与运行 mcp-server（不可违反）

- 当 `src/` 中的 C / C++ 等源码发生改动、需要重新 `build` 出新的 `mcp-server.exe` 时，
  **必须先停掉当前正在运行的 `build-mcp\mcp-server.exe` 进程，再重新构建并启动它**。
  - 原因：Windows 下正在运行的 exe 文件被 OS 锁定，链接器无法覆盖写入，会导致
    `cannot open output file mcp-server.exe: Permission denied` 的链接失败。
  - 因此重新构建前，务必先在启动它的终端（如 PowerShell）按 `Ctrl+C` 结束该进程；
    若进程已脱离终端，则通过任务管理器 / `taskkill /f /im mcp-server.exe` 结束后再构建。
  - 构建成功、必要文件（含 `web/`）同步到 `build-mcp/` 后，再重新启动
    `.\build-mcp\mcp-server.exe --ws 18765` 供前端连接。
  - **完整构建 = 编译 C++（CMake 会自动把 `web/` 复制到 `build-mcp/web/`，见
    `CMakeLists.txt` 的 POST_BUILD 步骤）+ 前端同步（`./sync-web.sh`，在复制基础上
    自动刷新 `?v=` 缓存号）**。C++ 未改动、只动前端时，也**必须**跑 `.sync-web.sh`，
    它就是“前端构建”这一步（详见下方「前端改动必须同步到 build-mcp/web」）。

## 前端改动必须同步到 build-mcp/web（不可违反）

- 运行中的 `mcp-server` 实际是从 **`build-mcp\web\`**（即 exe 同级 `web/` 目录）提供页面的，
  **不是** 仓库里的 `web\` 源目录（`build-mcp/` 整体被 `.gitignore` 忽略，只在构建时由
  `CMakeLists.txt` 的 `copy_directory` 步骤从 `web/` 复制而来）。因此**只改 `web/` 而不触发
  构建，运行时页面不会变化**。
- 每当改动 `web/`（含 `index.html`、`src/*.js`、`styles.css` 等）后，必须**立即把
  `web/` 整体同步到 `build-mcp/web/`**。规范的“前端构建”命令是一条脚本，复制 + 自动刷新
  缓存号一步完成：
  - `./sync-web.sh` —— 把 `web/` 复制到 `build-mcp/web/`，并改写其中所有 `?v=<token>`
    为新的时间戳 token（破除浏览器缓存）。**优先用它，不要手动 cp + 手改 ?v**。
  - Windows 上没有 bash（`bash ./sync-web.sh` 报 `execvpe(/bin/bash) failed`）时，用
    **`python sync-web.py`**：效果与 `sync-web.sh` 完全相同（同样整体复制 + 刷新 token）。
  - **严禁**用 PowerShell `Get-Content -Raw` + `[IO.File]::WriteAllText` 代替同步：PowerShell
    5.1 会用系统 ANSI 代码页（中文 Windows 为 GBK）读取无 BOM 的 UTF-8 文件，中文会被写成
    `mpq閰嶇疆` 这类乱码，并让 JS 报 `Uncaught SyntaxError: Invalid or unexpected token`
    （整个模块不执行，页面按钮全部失灵）。`sync-web.py` 全程按字节处理，不会改编码。
  - 手动等价命令（不推荐，易漏缓存号）：`cp -r web build-mcp/web` 后 `sed -i -E
    's/\?v=[A-Za-z0-9]+/?v=NEWTOKEN/g' build-mcp/web/index.html build-mcp/web/src/main.js`。
  - 该同步与是否重新编译 C++ 无关：即便只改了前端，也要同步；C++ 未改动时无需停 exe、也无需
    重新构建，但静态文件已落盘，浏览器刷新即可加载。
- **版本号防缓存（不可省略）**：`index.html` 与各 `src/*.js` 的 `<script>/import` 上都带
  `?v=<token>` 查询串。前端文件每次变更后，必须让相关的 `?v=` token 变化。`sync-web.sh`
  已自动处理；若手动改，必须把 `index.html` 引用的 `styles.css` 与 `src/main.js`、以及
  `main.js` 内 `import` 的 `client.js/catalog.js/viewer.js` 的 token 一起刷新，否则浏览器
  沿用旧缓存，即便 `build-mcp/web` 已更新也“刷不到”。
- 校验：同步后用 `grep -nE 'id="entityType"|value="entity"' build-mcp/web/index.html`
  （或对应本次改动的标志串）确认 `build-mcp/web` 已含最新内容；并在浏览器硬刷新（Ctrl+F5）。