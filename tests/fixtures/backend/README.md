# Backend Fixture Directory

Real CorridorKey runtime material is not committed to this repository.

For local CPU backend validation, create private files under:

```text
tests/fixtures/backend/private/
```

Required private files:

- `.corridorkey-backend-fixture` containing `fixture-only`
- `source.ckfb`: tiny source frame blob using the sidecar `CKFB` contract
- `alpha.ckfb`: matching one-channel alpha hint frame blob

The local fixture may also use a local `CORRIDORKEY_REPO` checkout containing
`corridorkey_ofx_adapter.py` and a local `CORRIDORKEY_MODEL_DIR` with compatible
test-only manifests. Do not commit private plates, model weights, checkpoints,
render outputs, project names, or absolute media paths.

If these private paths are absent, `scripts/run_cpu_backend_fixture.py` must write
a `blocked_backend` diagnostic instead of reporting CPU inference as passed.
