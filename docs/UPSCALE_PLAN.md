# AI 高清化（补值超分）实现规划

> 本规划基于「方向 A：标准格式导入/导出」并叠加 AI 补值超分。合规约束见
> [docs/COMPLIANCE.md](COMPLIANCE.md)——核心结论：**超分只在浏览器前端做，仓库零权重、
> 产物留本地、仅超分不做生成式**。

---

## 架构决策

- 超分**完全在浏览器前端**执行；C/C++ 后端只负责解码出 RGBA，不引入任何 ML 推理库
  （保持纯 C/C++、Node-free 的本色）。
- **双轨**：
  - 算法超分（最近邻 / 双线性 / Lanczos / xBR）：零权重、零依赖，先行上线；
  - AI 超分（waifu2x / ESRGAN 等）：用户自备 `.onnx` 模型，浏览器内 ONNX Runtime Web 推理。
- 严格限定 **image-to-image 超分**，禁生成式（diffusion / 文生图）重绘。

---

## 现状（基于实际代码）

- 解码：`src/tools/sprite_tools.cpp` 的 `sprite.decode` 把 `.clx` / `.cel` / `.cl2` / `.pcx`
  解出 RGBA 帧（base64 回传）；`src/decoders/*` 为各格式解码器。
- 前端渲染：`web/src/viewer.js` 已是 `rgba_base64 → atob → ImageData → putImageData` 的
  最近邻绘制；`web/index.html` 提供 6 种浏览模式，sprite 类模式（catalog / character /
  monster / combat）已能拿到逐帧 RGBA。
- 后端 PNG 编码：`src/decoders/png.cpp` 的 `encodePNG` 用于场景导出；前端导出直接走
  Canvas `toBlob`，无需后端参与。
- 合规护栏见 `docs/COMPLIANCE.md` §4 实现护栏清单。

---

## P-U0 前端导出管线（打底，无 ML）

- `web/` 增加「导出」面板（sprite 类模式可用）。
- 复用 `sprite.decode` 的 RGBA，前端离屏 canvas 组装。
- 导出 PNG：支持「逐帧独立 PNG」与「雪碧图（sprite sheet）+ JSON 帧表」两种。
- 导出 JSON：源名、原始尺寸、调色板引用、每帧偏移 / 尺寸、模式。
- 通过 blob URL **本地下载**，不上传、不落地服务端。

## P-U1 算法超分（零依赖、零权重，先行）

- 模式：最近邻整数倍（×2 / ×4，原味像素颗粒）/ 双线性 / Lanczos / xBR（像素风友好）。
- 实现：WebGL shader 或 Canvas2D；最近邻模式复用 `viewer.js` 的 ImageData 最近邻重采样。
- 验收：导出 PNG 尺寸 = 原尺寸 × factor；最近邻模式逐像素保真。

## P-U2 AI 超分（ONNX Runtime Web + 用户自备模型）

- 运行时：ONNX Runtime Web（WASM / WebGPU）。运行时 JS 走 CDN 或随 `web/` 附带
  （是代码非权重，合规允许）。
- **模型 `.onnx` 必须由用户 file picker 提供，绝不入库。**
- UI：模型文件选择 + 放大倍数 + 预处理（归一化 / 通道，按模型定）+ 后处理（裁到目标尺寸）。
- 推理：逐帧 RGBA → 张量 → ORT 推理 → 后处理 → 高清 RGBA → 走 P-U0 导出。
- 验收：同图多次推理结果稳定；产物仅存本地；无网络回传。

## P-U3 与方向 A 对齐

- 超分产物直接复用 P-U0 的 PNG + JSON 导出。
- 方向 A 的「导入回 HF MPQ」为另一任务（需 StormLib 写入支持），不在此清单，单独规划。

---

## 合规护栏（落地时必须，摘自 COMPLIANCE.md §4）

- 仓库 / release **零权重、零样本图、零放大产物**。
- 模型**仅来自用户本地文件**；UI 明示「请使用你拥有 / 有权使用的模型」。
- 产物**本地下载**，项目不中转、不托管。
- 仅超分，**不引入生成式重绘**。
- README / 插件文档复用 COMPLIANCE 声明文案。

---

## 测试

- 前端：Playwright / node 无头测试，验证放大后尺寸正确、最近邻模式像素一致、
  AI 模式不崩、PNG / JSON 可下载。
- 后端：超分在前端，现有三套 C++ 测试（unit / core / integration）不受影响，仍按规跑全绿。

---

## 优先级

P-U0 → P-U1（零风险、可立即出「前后对比」demo 物料）→ P-U2（AI，需用户自备模型）。
建议先发 U0 + U1 引流，U2 作进阶。
