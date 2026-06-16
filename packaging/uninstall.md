# CorridorKey OFX Uninstall Procedure

The distribution includes `packaging/uninstall_corridorkey_ofx.py` as the uninstaller artifact.
It removes install artifacts and preserves user model/log data by default.

## Default Behavior

```sh
python3 packaging/uninstall_corridorkey_ofx.py --dry-run
python3 packaging/uninstall_corridorkey_ofx.py --yes
```

Default targets by platform:

| Platform | Plugin bundle | Sidecar install files | User data preserved by default |
| --- | --- | --- | --- |
| macOS | `/Library/OFX/Plugins/CorridorKey.ofx.bundle` | `/Library/Application Support/CorridorKey OFX/sidecar` | `~/.corridorkey-ofx` |

## Opt-In Purge

Use `--purge-user-data` only when the user explicitly asks to remove local model/log data:

```sh
python3 packaging/uninstall_corridorkey_ofx.py --yes --purge-user-data
```

The purge option removes `models`, `logs`, `cache`, and `support-bundles` under the configured user data root, then removes the root only if it is empty.

## Test Overrides

For test installs, pass explicit paths:

```sh
python3 packaging/uninstall_corridorkey_ofx.py \
  --yes \
  --plugin-bundle /tmp/ck-test/OFX/Plugins/CorridorKey.ofx.bundle \
  --sidecar-install-dir /tmp/ck-test/sidecar \
  --user-data-root /tmp/ck-test/.corridorkey-ofx
```
