# macOS Packaging Notes

## Install Paths

- Default OpenFX path: `/Library/OFX/Plugins/CorridorKey.ofx.bundle`
- Local override: set `OFX_PLUGIN_PATH` to a directory containing `CorridorKey.ofx.bundle`.
- Optional separate sidecar install path, if an installer externalizes runtime files: `/Library/Application Support/CorridorKey OFX/sidecar`.
- User data root: `~/.corridorkey-ofx`.

## Signing, Notarization, And Gatekeeper

Distributed macOS packages should record signing and notarization status:

1. Sign `Contents/MacOS/CorridorKey.ofx` and any helper/runtime binaries.
2. Sign the `.ofx.bundle` after all contents are final.
3. Notarize the installer/archive and staple the notarization ticket where applicable.
4. Verify Gatekeeper on a clean macOS target with the exact packaged artifact.
5. Publish only redacted status summaries under `docs/public/`.
