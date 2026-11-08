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
| `src/decoders/` | 格式解码器（palette / clx / cel / cl2 / pcx） |
| `src/tools/` | Tool Registry + `mpq_tools` + `sprite_tools` |
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
| `spell.list` | 从 DevilutionX 数据表（`spelldat` / `misdat` / `missile_sprites` / `monstdat`）合成**玩家技能与怪物远程攻击的全阶段视觉目录**：每个条目按 施法 → 投射 → 命中 → debuff 四阶段组织；`sprite` 层给出编号 `missiles\{name}{N}.cl2` 文件清单 + 显式 `width`（编号文件**不能**用 `width:"auto"`），`trn` 层给出目标 debuff 的调色板重映射；另附 `impact_catalog` 候选命中特效。每条还带 `name_zh`（中文名，取自 `Translations/zh_TW.po`→`zh_CN.po` 的 `msgctxt "spell"`，无翻译则回退英文名）与 `description`（按已解析阶段链 + 玩家法力/智力消耗合成的中文说明）。可用 `source` / `id` 过滤（`src/tools/sprite_tools.cpp`、`src/tools/spell_tables.h`） |

请求示例（JSON-RPC 2.0，stdio 与 WS 同构）：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call",
 "params":{"name":"sprite.decode",
           "arguments":{"archive":"devilutionx.mpq","name":"data\\health.clx",
                        "palette_name":"ui_art\\diablo.pal"}}}
```

## 测试

后端各单元均配合单元测试，分三个套件，全部在 `out/` 下，用 MinGW-w64 的 `g++` 直接构建运行。
（注意：`GLOB_RECURSE` 仅用于 CMake 的 `mcp-server`；测试用 g++ 单文件直接编译，新增源文件无需重跑 cmake。）

### 1. 解码器 + base64（无需 MPQ / StormLib）

```bash
g++ -I src -std=c++17 out/unit_tests.cpp \
    src/decoders/clx.cpp src/decoders/cel.cpp src/decoders/cl2.cpp \
    src/decoders/pcx.cpp src/decoders/palette.cpp src/decoders/png.cpp \
    src/decoders/sprite_width.cpp \
    -static-libgcc -static-libstdc++ \
    -o out/unit_tests.exe && out/unit_tests.exe
```
覆盖：CLX 解码、CEL→CLX / CL2→CLX 往返、**CL2/CEL 畸形输入与错误 `width` 拒绝（不崩溃）**、**`spell_tables.h` 纯 join（TSV/CRLF、编号文件生成、AI→导弹、阶段派生）**、PCX 解码、调色板加载、base64 编解码往返。
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
    src/tools/mpq_tools.cpp src/tools/sprite_tools.cpp \
    src/decoders/palette.cpp src/decoders/clx.cpp src/decoders/cel.cpp \
    src/decoders/cl2.cpp src/decoders/pcx.cpp src/decoders/sprite_width.cpp \
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
**`spell.list` 全阶段目录**（51 技能 / 14 怪物攻击、Fireball 四阶段、Stone Curse TRN、
Fire Wall 双文件、Flash 双层、Torchant→Fireball、default→Arrow）。
源码路径：`src/engine/archive.cpp`、`src/tools/mpq_tools.cpp`、`src/tools/sprite_tools.cpp`、
`src/tools/spell_tables.h`、`src/decoders/cl2.cpp`、`src/decoders/cel.cpp`、
`src/decoders/sprite_width.cpp`；测试脚本 `out/integration_tests.cpp`。

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
`PNG`（`src/decoders/png.cpp`，自带**无依赖 zlib** 的存储块编码器，无需 `-lz`）。

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

- 源码路径：`mpq-info/render_sprite.cpp` + `src/decoders/png.cpp`；PNG 编码器含于单元测试
  （writePNG → 回读校验 round-trip）。
- 自动查表覆盖 5 类精灵：`plrgfx`（按 `sprites.tsv` 的 `stand`/`walk`/`attack`(含 `bow`)/`swHit`/`lightning`/`fire`/`magic`/`death`/`block` 列）、`monsters`（按 `monstdat.tsv` 的 `width`）、`missiles`（按 `missile_sprites.tsv` 的 `width`）、`objects`（按 `objdat.tsv` 的 `animWidth`）、`towners`（按 `towners.tsv` 的 `animWidth`）。`items` 无宽度表，需手填。
- 已验证：`plrgfx\warrior\wlu\wluas.cl2` 自动查表取宽 96 渲染为 96×96 战士立姿精灵（含盾牌），
  80 帧（8 方向 × 10）均可解码无错位；怪物 `monsters\acid\acida.cl2`（宽 128）、导弹
  `missiles\arrows.cl2`（宽 96）、物件 `objects\armstand.cel`（宽 96）、NPC `towners\drunk\twndrunk.cel`
  （宽 96）均经自动查表渲染成功。

## 开发文档

更详细的架构与阶段计划见 `docs/ARCHITECTURE.md` 与 `docs/PROJECT.md`。
