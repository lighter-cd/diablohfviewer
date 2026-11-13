# 场景渲染前端化设计（scene.tiles + 前端 SceneRenderer）

> 本文档描述将「地图绘制」从后端（`scene.build` 整张合成 PNG）迁移到前端（后端发
> 图块索引图集 + 布局，前端按等距投影拼接并本地着色/开门）的设计方案。
> 对应需求：后端只提供图块内容与地图布局、前端拼接；光照转移到前端；地牢门洞的
> 开关在前端完成。编码方式已确认采用 **索引 atlas**（后端解码成 32×32 帧，atlas
> 像素的 R 通道存调色板索引、A 存透明标记；CEL 复杂解码留在 C++，前端只做 blit + 查表
> 着色）。

---

## 1. 背景与目标

当前 `scene.build`（`src/tools/scene_tools.cpp`）在后端完成：MPQ 解包 → 组装 dPiece
网格 → 解码 CEL 成 RGBA → `buildLightMap` 烘焙 16 套调色板 → `renderMap` 整张合成 →
输出 1 张 native PNG + overlay PNG + 元数据 JSON。前端 `web/src/main.js` 的 `loadScene`
只把 PNG 贴到 `sceneBase`/`sceneOverlay` 两张 canvas。

痛点：

- 任何交互（开/关门、调光照、拖动火把、战争迷雾揭示）都要重跑 `scene.build`，服务端
  往返 + 重新合成整张图，无法实时。
- 门洞开关目前由后端 `applyL2ClosedDoors` 把 micro 索引烘焙进网格，开门必须重新生成。
- 光照在后端一次性烘焙成 RGBA，前端拿到的已是不可变的像素，无法再按格调暗。

目标：后端改为「**只解码、不合成**」，把最小可拼装的图块数据与布局下发给前端；前端持有
完整图块图集与投影参数，本地完成两遍等距绘制、逐格光照着色、门开关切换。

---

## 2. 目标架构

```
后端 scene.tiles (新工具 / scene.build 的 format:"tiles" 模式)
  MPQ 解包（复用 loadSceneAssets）
  → 组装 dPiece 网格（复用 buildDungeonScene / buildTownScene）
  → 解码 level CEL 成「索引帧」(新增 decodeReencodedFrameAsIndex)
  → 打包成 atlas PNG（R=调色板索引, A=透明标记）
  → 产出 layout.json：
       projection / microGrid / atlas 映射 / palette(256*4)
       lightTables(16*256) / solid+blockLight+blockMissile+transparent
       / light[]（或前端实时算）/ special 索引 atlas + 映射 / doors[]
  ↓  （一次拉取，浏览器缓存）
前端 SceneRenderer (web/src/scene-render.js，新增)
  持有 atlas ImageData + layout
  复刻 renderMap 的 sceneProjection + cellX/colTop/slotTop + 两遍画家顺序
  逐格从 atlas blit 子矩形，按该格 light[i] 查 lightTables 把索引→RGBA
  门开关：本地改 microGrid 并重绘该格；光照滑块：本地重着色
```

PNG 模式保留：用于非交互导出 / 缩略图，与 tiles 模式互不干扰（双轨并行）。

---

## 3. 数据契约

### 3.1 索引 atlas（图块）

- 每个 micro 件、每个 block `b ∈ [0, microLen)` 解码为 **32×32 的调色板索引帧**。
- 打包成一张 PNG，`k` 帧按网格排列（建议 64 帧/行 → 宽 2048，高 `ceil(k/64)*32`）。
- 像素编码：**R = 调色板索引（0..255），G=B=0，A = 255（该像素被绘制）/ 0（透明）**。
  索引 0 与透明靠 A 区分（Diablo CEL 中索引 0 即透明色，A=0 标记之）。
- **只打包网格实际引用到的 micro**（后端 `grid.micro` 去重后），预设房间通常只引用
  几十件，`l1_rnd2` atlas 预计 < 100KB；最坏 town 全量 2012 件约 5–12MB，本地一次拉取。

C++ 侧改动（低风险，复用现有解码）：

```cpp
// 在 level_cel.cpp 新增索引输出变体，复用现有 RLE 解码，只是 CopyRun 不再做 pal[] 映射
int decodeReencodedFrameAsIndex(const ReencodedLevelCel& cel, uint32_t frameIndex,
                                TileType forced, std::vector<uint8_t>& indices /*32*32*/,
                                std::vector<uint8_t>& drawn /*32*32, 0/1*/, int& outH);
```

打包：遍历去重后的 micro 集合，对每个 `(microIndex, block)` 调上述函数，把
`indices[k]` 写入 atlas 像素 R、`drawn[k]*255` 写入 A，并记录
`(microIndex, block) → (atlasX, atlasY)` 矩形。

### 3.2 layout.json schema

```jsonc
{
  "scene_id": "l1_rnd2",
  "micro_len": 10,
  "projection": { "minX": -672, "minY": -159, "width": 1408, "height": 736 },
  "grid_width": 12, "grid_height": 12,
  "micro_grid": [ 12, 0, 540, ... ],          // gw*gh，0 或 kNoMicro(65535) 表示空
  "atlas": {
    "url": "/scenes/<id>_<tag>_tiles.png",      // 索引 atlas（R=索引, A=透明）
    "tile": 32,
    "cols": 64,
    "frames": {                                 // (microIndex,block) -> 矩形
      "12:0": [0,0], "12:1": [32,0], "12:2": [64,0], ...
    }
  },
  "palette": [ /* 256*4 RGBA */ ],
  "light_tables": [ /* 16 组，每组 256 字节：LightTables[level][index] */ ],
  "light_style": 0,
  "solid":        [ /* gw*gh 0/1 */ ],
  "block_light":  [ /* gw*gh 0/1 */ ],
  "block_missile":[ /* gw*gh 0/1 */ ],
  "transparent":  [ /* gw*gh 0/1 */ ],
  "light":        [ /* gw*gh 0..15，仅 lighting 模式；可省略改由前端算 */ ],
  "special":      [ /* gw*gh，dSpecial id（0=无） */ ],
  "special_atlas": {
    "url": "/scenes/<id>_<tag>_special.png",
    "frames": { "1": [0,0], "2": [32,0], ... }   // dSpecial id -> 矩形
  },
  "doors": [                                     // 门洞开/关 micro 对
    { "x": 8, "y": 6, "open": 12,  "closed": 537 },
    { "x": 12, "y": 14, "open": 12, "closed": 537 }
  ],
  "background": [0,0,0,255]
}
```

`projection` 与 `frames` 映射让前端无需理解 CEL 内部布局，只做「取矩形→贴到等距位置」。

### 3.3 special 精灵 atlas

- `l1s.cel` / `l2s.cel` / `towns.cel` 同样解码成索引帧，单独 atlas + `frames` 映射（按
  dSpecial id）。
- **关键简化**：当前后端为地牢 special 预解码 16 套 RGBA（`specialSpritesLit`，见
  `level_render.cpp:395-414`）。改为索引 atlas 后，special 与地形共用同一套
  `lightTables`，前端按每格 `light[i]` 着色即可，**16 份拷贝消失**，体积与复杂度双降。

---

## 4. 后端改动（scene.tiles）

- 新增 `toolSceneTiles`（或在 `toolSceneBuild` 加 `format` 参数），复用
  `loadSceneAssets` 解析、`reencodeLevelCel` 重编码。
- 新增 `decodeReencodedFrameAsIndex`（§3.1）与 atlas 打包逻辑（PNG 编码复用
  `encodePNG`，注意写的是 R=索引 / A=透明，非真实颜色——可用一个临时 256×1 调色板让
  `encodePNG` 把它当作灰度图，或直接以 RGBA 形式编码 R 通道）。
- `doors[]`：对 Catacombs/Caves/Hell 在 `buildDungeonScene` 已能识别门洞 micro
  （`12/540`、`16/541` 等），按 `AddL2/L3/L4Objs` 的 open/closed micro 对导出。
- 缓存 key 沿用 `buildCacheKey` 思路，加 `format` 维度（不与 PNG 缓存冲突）。
- 输出：atlas PNG（HTTP 端点）+ layout JSON（JSON-RPC 返回，atlas 走 URL 避免塞进 payload）。

---

## 5. 前端 SceneRenderer 算法（renderMap 移植）

`web/src/scene-render.js`，纯 Canvas2D + ImageData，复刻 `level_render.cpp`：

```js
// 投影（复刻 sceneProjection）
const Hm = (microLen/2) * 32;
const proj = { minX: -(gh-1)*32, minY: -Hm+1,
               width: (gw-1)*32+64 - minX, height: (gw+gh-2)*16 - minY };
const cellX = (i,j) => (i-j)*32 - proj.minX;
const colTop = (i,j) => (i+j)*16 - proj.minY - Hm + 1;
const slotTop = (ct,b) => ct + (Hm - (b+1)*32);

// 预构建 16 张「着色调色板」：shadedPal[level][idx] = palette[ lightTables[level][idx] ]
// （一次性，16*256 查表）

function shade(idx, level) {            // 索引 -> RGBA
  const s = lightTables[level][idx];
  return [palette[s*4], palette[s*4+1], palette[s*4+2], 255];
}

// 两遍绘制（复刻 renderMap）
// Pass1：每个 cell 若是 floor（!solid && !blockMissile），画 mt[0]/mt[1] 为左/右三角形菱形
// Pass2：非 floor 画 mt[0]/mt[1] 真实 TileType；再画 mt[2..microLen-1] 墙体栈；最后 dSpecial 覆盖层
// 每格用该格 light[i]（或 FullyLit=0）选 shadedPal
// blit：从 atlas 取 (microIndex,block) 矩形（ImageData 子区），按像素 shade 后写入目标 canvas
```

正确性要点（已在后端验证的语义，须原样移植）：

- **画家顺序**：两遍均按 `i+j` 升序（远→近），保证近处覆盖了远处。
- **floor 强制三角**：`DecodeSquare` 等真实形状在 floor 位仍按左/右三角形画
  （`renderMap:337-341` 的 `side==0?LeftTriangle:RightTriangle`）；`TransparentSquare`
  例外走真实解码（`renderMap:333-335`）。
- **dSpecial 底对齐**：精灵底部锚定 cell 基（`blitSpriteBottom`，`renderMap:408-413`）。

回归校验：用现有 `scene.build` 的 PNG 输出做**逐像素对照**，A/B 脚本（见此前
lighting/门 修复用过的 PNG 解码对比）确保 SceneRenderer 与后端合成一致。

---

## 6. 三问落地

### 6.1 拼接绘制（后端供图块+布局，前端拼）
见 §3–§5。后端发索引 atlas + layout，前端按投影逐格 blit。地牢/城镇/ hell 均走同一
渲染器，仅 `micro_len`、`light_style`、`special_cel` 不同。

### 6.2 光照前端化
- atlas 存**索引**，前端逐像素 `shadedIndex = lightTables[light[i]][index]` 再取
  `palette[shadedIndex]`。`lightTables`(4KB) + `palette`(1KB) 随 atlas 下发，零额外带宽。
- `buildLightMap`（ambient + 点光源 + 软衰减）是无资源依赖的纯数学，可一并移植到前端
  → **实时环境光滑块 / 拖动火把**零服务端往返。本期至少先下发预计算的 `light[]` 数组，
  后续再上实时编辑。
- 与当前后端「预烘焙 16 套 RGBA」相比，索引方案把多套拷贝压成运行期查表，**地牢 special
  的 16 份 `specialSpritesLit` 也一并消除**（§3.3）。

### 6.3 门洞开关前端化
- 后端在 `doors[]` 给出每扇门的坐标与 open/closed micro 对（来自 `AddL2/L3/L4Objs` 分析）。
- 前端持有可编辑的 `microGrid`；开/关 = 本地把该格 micro 索引在 open↔closed 间互换，并
  **仅重绘该格所在列**（atlas 已含两套帧），无请求、无等待，可做开合过渡动画。
- 这是相对当前服务端烘焙方案的**确定性改进**：开门不再需要重跑 `scene.build`。

---

## 7. 体积与性能评估

| 项 | 当前（scene.build） | 迁移后（scene.tiles） |
|---|---|---|
| 单次传输 | 300KB–858KB（native PNG） | atlas + JSON：预设 < 200KB，town 全量 ~5–12MB |
| 拉取频次 | 每次交互都重拉/重算 | 仅一次，浏览器缓存 |
| 开门 | 重跑后端 | 本地互换 micro + 重绘 1 格 |
| 光照调整 | 重跑后端 | 本地查表重着色（~数十 ms） |
| CEL 解码位置 | 后端 | 后端（索引帧），前端只 blit |

性能：预设房间数十万像素、全图 town ~18M 像素。Canvas2D 逐像素 shade 在 JS 中约数十毫秒
量级，交互足够；若需更顺滑可后续上 WebGL（不在本期）。门开关只重绘单格（~10K 像素），瞬时。

---

## 8. 分阶段实施

1. **Phase 0 — 后端 scene.tiles**：`decodeReencodedFrameAsIndex` + atlas 打包 + layout.json
   （含 `doors`、`light_tables`、`special_atlas`）。复用现有解析与缓存 key 逻辑。
   ✅ **已完成**（2026-11-13）：`toolSceneTiles` 落地，`src/decoders/clx.cpp`/`clx.h` 新增
   `decodeClxIndex`/`decodeReencodedFrameAsIndex`，`level_cel.cpp` 负责重编码，
   `scene_tools.cpp` 完成 tiles atlas + special atlas 打包 + layout 输出。
2. **Phase 1 — 前端 SceneRenderer**：复刻 `renderMap` 两遍 + 投影，**与当前 `scene.build`
   PNG 逐像素对齐**验证。
   ✅ **已完成**（2026-11-13）：`web/src/scene-render.js` 复刻两遍等距绘制、逐格 dLight
   着色、dSpecial 底对齐覆盖层；`tests/regress_scene_tiles.mjs` 逐字节对照后端
   `scene.build` 合成 PNG。
   - **回归结果**：`l1_rnd2` 逐字节 **100.0000%**（0 字节差异，4689152/4689152）；
     `l1_skngdo`、`l2_blind`、`l3_anvil`、`l4_diab1` 同样 100%；`l1_rnd2` 在
     `lighting:true`（lit 模式）下亦 100%。
   - **关键修复**：special atlas 打包原先在增长时调用 `acc.assign(...,0)` 会整体重分配并
     清零缓冲、改变行跨度，导致已放置帧错位/清零；改为「先布局后写入」两遍打包
     （shelf 行宽 2048，逐帧 memcpy 子矩形），`ATLAS diff=0`。
3. **Phase 2 — 接光照（实时、零服务端往返）**：✅ **已完成**（2026-11-13）。
   - `web/src/scene-light.js` 完整移植 DevilutionX `level_light.cpp` 的 `buildLightMap`
     （16 级 falloff + `LightConeInterpolations[8][8][16][16]` + 四向旋转环
     `rotateX/rotateY` + `kRotationSines=[0,1,0,-1]` + offsetX/Y 归一化 + blockX/Y 进位），
     输入 `gridW,gridH,ambient,lights[],softFalloff`，输出每格 0..15 的 `Uint8Array`。
   - 后端 `scene.tiles` 缓存 key 升 **v6**，layout 始终下发 `default_ambient` / `lights[]`
     / `soft_falloff`（即便 `lighting:false` 也下发，供前端自由重算），`light_tables`
     16×256 一并下发；前端 `renderScene` 新增 `opts.light` 优先级（覆盖服务端
     `layout.light`）以 `light_tables[level][index]` 逐格重着色。
   - 查看器交互：新增「环境光」滑块（0..15 实时重算 dLight 并重绘）与可拖拽「火把」标记
     （逆等距投影拾取微格 + `offset_x/y=round(frac*8)`，零服务端往返）；底图**始终以
     `lighting:false` 请求** `scene.tiles`，确保前端完全掌控、关灯时不被服务端
     `layout.light` 回退点亮。
   - **验收**：WS `scene.tiles`→atlas HTTP 端到端 200 + 合法 PNG；前端 `buildLightMap`
     与后端预计算 `layout.light` 在 l1_rnd2/l2_blind/l3_anvil/l4_diab1/l1_skngdo 上
     **100% 一致**（484/484 格）；前端 `SceneRenderer`（unlit）与后端 `scene.build`
     逐像素 **100%**（`tests/regress_scene_tiles.mjs` 已改为按投影尺寸匹配 build PNG，
     规避多缓存变体误选）；三套 C++ 测试全绿、`node --check` 通过、i18n 1371 断言通过。
4. **Phase 3 — 门开关本地切换（实时、零服务端往返）**：✅ **已完成**（2026-11-14）。
   - 后端 `scene.tiles` 缓存 key 升 **v7**，atlas 打包时额外收入门**开启**态 micro（L2 537/539→12/16；网格只存闭合态，开启态不在 `micro_grid` 里，必须打进 atlas 否则切换时门消失）。`layout.doors[]` 仍按 537/539↔12/16 发出关闭/开启 micro 对（Catacombs 门洞无 dSpecial 覆盖层，`open_special/closed_special=0`，开关是纯 micro 互换）。
   - 前端 `web/src/scene-render.js` 新增 `opts.microOverride`（`Map` 或 `{idx->microId}`），Pass1/Pass2 读取 micro 时优先用覆盖值，其余格保持 layout micro。
   - 查看器：新增「门编辑」开关 → 地图门显示可点击标记（绿=开/红=关），点击即在 `closed↔open` 间本地互换 micro 并重绘，**零服务端往返**；标记带开关脉冲 + 底图淡入过渡动画；标记是 `#sceneLayers` 子节点，随平移/缩放自动对齐（与火把一致）。`applySceneTransform` 在平移/缩放时重定位门标记；视口 `mousedown` 对门标记与火把 alike 跳过平移。
   - **验收**：`tests/regress_scene_door.mjs` 验证门 microOverride 路径——覆盖为开启 micro 图像必变、覆盖为闭合 micro（==layout）图像不变；v7 `l2_blind` tiles 实测 `doors[]`（2 扇，537→12）且开启 micro 12 帧已打包；三套 C++ 测试 + i18n 1381 断言全绿、`node --check` 通过。
5. **Phase 4 — 切换查看器管线**：✅ **已完成**（2026-11-14）。
   - 场景查看器正式由 `scene.tiles` 全量接管：`loadScene` 不再调用 `scene.build` 取叠加层 / 元数据；基础图（前端 `SceneRenderer`）+ 叠加层（网格 / SOL 标志）均在浏览器本地绘制。`scene.tiles` 的 `layout` 本就下发 `solid`/`block_light`/`block_missile`/`transparent` SOL 标志与 `projection`，无需后端改动（缓存 key 维持 v7）。
   - 前端 `web/src/scene-render.js` 新增 `renderSceneOverlay`（按后端 `buildOverlay` 同投影 / 同槽位数学、step=1 原生分辨率移植：solid 红 / transparent 蓝 / blockLight 黄 半透明菱形 + 每 8 格白色参考网格），与 `renderScene` 输出逐像素对齐。元数据摘要改为 tiles 原生值（标题取自目录、`w/h` 原始宽高、`gw/gh` 网格、Solid 计数、光照），去掉后端专属的预览尺寸 / 缩放 / 透明像素字段。
   - `scene.build` 的 PNG 模式保留为「导出场景 PNG」按钮（反映当前光照开关，新标签页打开原生 PNG），并继续作为 `scene.build` 与前端渲染的回归基准。移除已无用的 `sceneDrawUrl` 辅助。
   - **验收**：`tests/regress_scene_overlay.mjs` 将前端 `renderSceneOverlay` 与后端 `scene.build` 原生叠加层 PNG 逐字节对照——`l1_rnd2`/`l2_blind`/`l3_anvil`/`l4_diab1`/`l1_skngdo` 均 100%；i18n 1396 断言全绿、`node --check` 通过、无后端改动。

---

## 9. 风险与回归

- **主要风险**：JS 端画家顺序 / 三角强制 / TransparentSquare 特例的忠实移植。缓解：用现有
  `scene.build` 输出做逐像素对照（A/B 脚本已具备）。
- **atlas 体积**：town 全量偏大，靠「仅打包引用到的 micro」+ 浏览器缓存缓解；必要时可
  按 micro 区间分片 atlas。
- **二进制透明度**：索引 0 与透明靠 A 通道区分，已在 §3.1 约定，前端跳过 A=0 像素。

---

## 10. 与现有 PNG 模式兼容

- `scene.build` 的 PNG 输出（native / preview / overlay）**保留不动**，继续服务导出与
  已有测试（unit/core/integration 三套测试不破坏）。
- 新增 `scene.tiles` 为增量能力；前端场景查看器在 Phase 4 才切换，此前双轨并行。
- `scene.path`（A* 寻路）基于 SOL 网格，与渲染无关，不受影响。
