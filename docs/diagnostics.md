# Diagnostics And Support Bundles

CorridorKey diagnostics are local-first. Logs, doctor output, and support bundles must
help a pipeline TD diagnose installation, model, backend, render, and IPC failures
without exposing frame contents, project names, raw media paths, or complete plaintext
project paths.

## Required Diagnostic Fields

Every support bundle diagnostics record must include:

- `plugin_version`
- `sidecar_version`
- host name and version when the host provides them
- OS name, release, and CPU architecture
- CPU summary and GPU summary, using `unknown` when unavailable
- backend and model status
- pixel format and frame size, using `unknown` when no render context is available
- timings
- warning codes
- error codes

## Privacy Rules

Diagnostics may include environment and status data, but must not include:

- frame contents or sampled pixel values
- project names, show names, or shot names
- raw media paths
- complete plaintext project paths

Path-like values are replaced with `<redacted-path>`. Sensitive project/name fields
are replaced with `<redacted>`. Bare media filenames are replaced with
`<redacted-file>`.

## Support Bundle Sufficiency

A support bundle is sufficient only when all of these objective criteria pass:

- Minimum files exist: `manifest.json`, `diagnostics.json`, `doctor.txt`,
  `logs/redacted.log`, `manifest_status.json`, `backend_status.json`,
  `recent_errors.json`, and `redaction_proof.json`.
- `diagnostics.json` contains every required diagnostic field listed above.
- `redaction_proof.json` records successful path, project-name, and media-path
  redaction.
- `recent_errors.json` contains an `errors` array, even when it is empty.
- `manifest.json` records the bundle creation metadata.

Public release notes should summarize only pass/fail status and residual risks,
without including raw logs, host caches, media paths, or support bundle contents.

## Doctor

`scripts/doctor_dev_env.py` reports local environment status, OpenFX SDK
header availability, plugin/sidecar versions, host status when provided through
environment variables, runtime configuration booleans, and model/model source/install
status. It prints labels and status values only; configured paths are redacted or
reported as present/absent.

The OFX `Run Doctor` button runs the local doctor when the script is available and
updates status parameters with `running`, `completed`, `failed`, or `unavailable`.
The status surface does not display the generated report path.

## Host Button Actions

Opening OS folders or copying arbitrary files from inside a host can be unsafe or
unsupported. When a host-safe action is unavailable:

- `Open Log Folder` reports a documented fallback instead of launching a GUI action.
- `Copy Support Bundle` creates a local redacted support bundle when the packaged
  runtime and support bundle module are available. The status parameter reports
  `running`, `completed`, `failed`, or `unavailable`; it does not display the
  generated bundle path.

A TD can also create a local bundle outside the host with:

```bash
python3 -m sidecar.corridorkey_sidecar.support_bundle --output <support-output-dir>
```

When `Open Log Folder` is unavailable, use the support bundle command above or inspect
the configured local diagnostics directory from outside the host. Do not paste complete
project, media, or diagnostics paths into support tickets; include only the redacted
bundle contents.

The host actions intentionally avoid printing complete local paths in the plugin status
surface.
