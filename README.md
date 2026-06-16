<h1 align="center">CorridorKey OpenFX</h1>

<p align="center">
  <a href="README.md">English</a> · <a href="README.zh-CN.md">简体中文</a>
</p>

CorridorKey OpenFX is an intelligent keying plugin for professional post-production software. It packages CorridorKey's local inference capability as an OpenFX plugin, allowing supported OFX hosts such as Autodesk Flame and Blackmagic Design DaVinci Resolve to use intelligent keying inside timeline, node, and compositing workflows.

The plugin is designed for post-production workflows that need high-quality alpha generation, foreground separation, and compositing-ready outputs. It is suitable for people, objects, green-screen or blue-screen assisted keying, complex edge handling, and workflows that need keying preview and result writeback inside the host application.

## Original Project Notice

CorridorKey OpenFX is an OpenFX plugin wrapper around the capabilities of the original CorridorKey project. The original CorridorKey project provides the core intelligent keying model and inference capability, and is available at [nikopueringer/CorridorKey](https://github.com/nikopueringer/CorridorKey). This repository provides the OpenFX host integration, local sidecar invocation, packaging structure, and public usage documentation.

Copyright, license, and distribution terms for the original project, model weights, and related dependencies are governed by their respective upstream publishers. This repository does not include model weights and does not replace the original project's license notice.

## Product Positioning

CorridorKey OpenFX is not a standalone application. It is a local keying component embedded into professional post-production hosts. Users manage footage, parameters, and output inside the familiar host environment, while the plugin passes frame data to the local inference runtime and returns generated results to the host.

This preserves the workflow of a traditional OpenFX plugin while bringing intelligent model capability into existing color, compositing, and finishing pipelines.

## How It Works

The plugin has two parts:

- OpenFX plugin: handles host integration, parameter control, image input/output, and result writeback.
- Local sidecar: handles model execution, job queueing, cache, diagnostics, and runtime isolation.

Models and heavy runtime dependencies are not loaded directly into the host process. They run inside the local sidecar, reducing impact on host stability and making heavier inference dependencies easier to manage.

## Use Cases

- Run intelligent keying directly inside Autodesk Flame or Blackmagic Design DaVinci Resolve.
- Generate alpha or helper mattes for compositing, color, and finishing workflows.
- Preview, adjust, and write back keying results without leaving the host application.
- Bring local model capability into an existing OpenFX workflow.

## Usage

1. Add the `CorridorKey OpenFX` effect in the host application.
2. Use the image to key as the main input.
3. If available, provide a rough matte, GMask, Keyer result, or external alpha as the `AlphaHint` / alpha guide input.
4. In the parameter panel, choose `Screen Color`: `Auto`, `Green`, or `Blue`, depending on the shot.
5. Choose `Quality` to control preview and output quality.
6. Use `Output Mode` to inspect different results:
   - `Processed RGBA`: compositing output with alpha.
   - `Matte`: alpha-only view.
   - `Straight FG`: unpremultiplied foreground view.
   - `Alpha Hint View`: guide matte inspection.
   - `Checker Comp`: edge preview over a checkerboard background.
   - `Status`: plugin status view.
7. Adjust parameters based on edges, hair, translucent areas, and spill, then render or cache the result in the host.

## Architecture

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

The OpenFX plugin exchanges images and parameters with the host; the sidecar handles inference and runtime management. They communicate through local IPC, forming a layered structure with a lightweight plugin and an isolated inference process.

## Creation Notice

This project was created using OpenAI Codex Vibe Coding.
