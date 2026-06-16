# OpenFX Bundle Layout

CorridorKey uses a deterministic OpenFX bundle named:

```text
CorridorKey.ofx.bundle
```

The build script treats `--output` as a distribution directory and places the bundle under that directory:

```text
<output>/
  CorridorKey.ofx.bundle/
    Contents/
      Info.plist
      Resources/
        sidecar/
          corridorkey_sidecar/
            __init__.py
            protocol.py
            redaction.py
            server.py
      MacOS/
        CorridorKey.ofx
```

The package places the macOS plugin binary at:

```text
macOS:   Contents/MacOS/CorridorKey.ofx
```

## Install Location

Default OpenFX plugin install location:

```text
macOS:   /Library/OFX/Plugins
```

Local builds may be loaded by setting `OFX_PLUGIN_PATH` to a directory that contains `CorridorKey.ofx.bundle`. Do not point `OFX_PLUGIN_PATH` at `Contents` or at a platform binary directory.

## Dry Run Command

```sh
python3 packaging/build_bundle.py --dry-run --output build/dist
```

The dry-run command prints the expected tree without writing files and is safe to run before a plugin binary exists.

After a local plugin binary exists, build the bundle with:

```sh
python3 packaging/build_bundle.py --output build/dist
```
