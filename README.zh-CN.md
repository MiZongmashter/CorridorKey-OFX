<h1 align="center">CorridorKey OpenFX</h1>

<p align="center">
  <a href="README.md">English</a> · <a href="README.zh-CN.md">简体中文</a>
</p>

CorridorKey OpenFX 是一款面向专业后期软件的智能抠像插件。它将 CorridorKey 的本地推理能力封装为 OpenFX 插件，让 Autodesk Flame、Blackmagic Design DaVinci Resolve 等支持 OFX 的宿主可以在时间线、节点和合成流程中调用智能抠像能力。

插件面向需要高质量 Alpha、前景分离和素材合成的后期工作流。它适合用于人物、物体、绿幕/蓝幕辅助、复杂边缘处理，以及需要在宿主软件内完成抠像预览和结果写回的场景。

## 原始项目声明

CorridorKey OpenFX 是对 CorridorKey 原始项目能力的 OpenFX 插件化封装。CorridorKey 原始项目负责核心智能抠像模型与推理能力，项目地址为 [nikopueringer/CorridorKey](https://github.com/nikopueringer/CorridorKey)；本仓库负责 OpenFX 宿主集成、本地 sidecar 调用、打包结构和公开使用文档。

原始项目、模型权重及相关依赖的版权、许可证和分发条款以各自原始发布方为准。本仓库不包含模型权重，也不替代原始项目的许可声明。

## 产品定位

CorridorKey OpenFX 不是独立应用，而是嵌入专业后期宿主的本地化抠像组件。用户在熟悉的宿主环境中管理素材、参数和输出，插件负责把帧数据交给本地推理运行时，并把生成结果返回给宿主。

这种方式保留了传统 OFX 插件的工作习惯，同时把智能模型能力接入现有调色、合成和 finishing 流程。

## 工作方式

插件由两部分组成：

- OpenFX 插件：负责宿主集成、参数控制、图像输入输出和结果写回。
- 本地 sidecar：负责模型运行、任务队列、缓存、诊断和运行时隔离。

模型和运行时不直接塞进宿主进程，而是在本地 sidecar 中运行。这样可以减少对宿主稳定性的影响，也方便管理较重的推理依赖。

## 适用场景

- 在 Autodesk Flame 或 Blackmagic Design DaVinci Resolve 中直接进行智能抠像。
- 为合成、调色和 finishing 流程生成 Alpha 或辅助 matte。
- 在不离开宿主软件的情况下预览、调整和写回抠像结果。
- 将本地模型能力接入已有 OpenFX 工作流。

## 使用说明

1. 在宿主软件中添加 `CorridorKey OpenFX` 效果。
2. 将需要抠像的画面作为主输入。
3. 如有粗略 matte、GMask、Keyer 结果或外部 Alpha，可作为 `AlphaHint` / alpha 引导输入。
4. 在参数面板中选择 `Screen Color`，根据素材选择 `Auto`、`Green` 或 `Blue`。
5. 选择 `Quality` 控制预览和输出质量。
6. 使用 `Output Mode` 查看不同结果：
   - `Processed RGBA`：带 Alpha 的合成输出。
   - `Matte`：单独查看 Alpha。
   - `Straight FG`：查看未预乘的前景。
   - `Alpha Hint View`：检查引导 matte。
   - `Checker Comp`：在棋盘背景上预览边缘。
   - `Status`：查看插件状态。
7. 根据画面边缘、发丝、半透明区域和溢色情况调整参数，再在宿主中渲染或缓存结果。

## 架构

```text
Host Application
       |
       v
CorridorKey OpenFX Plugin
       |
       v
Local Sidecar
       |
       v
Model Runtime
```

OpenFX 插件负责与宿主交换图像和参数；sidecar 负责推理和运行时管理。两者通过本地 IPC 通信，形成轻量插件与独立推理进程的分层结构。

## 制作声明

本项目使用 OpenAI Codex Vibe Coding 制作。
