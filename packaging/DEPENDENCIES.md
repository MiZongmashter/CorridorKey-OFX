# CorridorKey OFX Dependencies

## Build-Time

- CMake 3.20+
- C++20 compiler
- Python 3.11+ for the sidecar and packaging scripts
- OpenFX SDK/support headers in `third_party/openfx`

## Runtime

- Host OpenFX implementation: Autodesk Flame 2026.x and Blackmagic Design DaVinci Resolve target rows summarized under `docs/public/host-gates.md`
- Sidecar Python runtime
- CorridorKey checkout or adapter source
- Model files with `model-manifest.json` and SHA-256 checksums
- Optional backend runtime: MLX

## Packaging Status

The packager copies sidecar source and model manifests when available. It does
not commit heavyweight Python/MLX runtimes or model weights to the source
repository. Release assets may provide runtime and model material separately.
