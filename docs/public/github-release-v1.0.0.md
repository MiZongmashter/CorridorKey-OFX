# CorridorKey Apple 1.0

Apple 1.0 is the public macOS Apple Silicon MLX package. Raw QA evidence,
host logs, media renders, support bundles, and host cache files are not included
in this source repository or release notes.

## Asset

| Item | Value |
| --- | --- |
| Package | `CorridorKey-macOS-1.0.0.pkg` |
| Size | 1,753,309,343 bytes |
| SHA-256 | `897bb1dfe908335a36e6673556ce92ad8e5c8577c70e69b7530f7095803814f5` |
| Signing | Unsigned |
| Notarization | Not notarized |

## Host Gates

| Host | Version | Result |
| --- | --- | --- |
| Autodesk Flame | 2026.2.1 | Pass: minimal scan/load and post-install five-mode checks |
| Blackmagic Design DaVinci Resolve Studio | 21.0.0.48 | Pass: five output modes |

## Install Notes

Verify the package checksum before installing:

```bash
shasum -a 256 CorridorKey-macOS-1.0.0.pkg
```

Expected SHA-256:

```text
897bb1dfe908335a36e6673556ce92ad8e5c8577c70e69b7530f7095803814f5
```

This package is unsigned and not notarized. macOS Gatekeeper may require an
explicit user approval flow.
