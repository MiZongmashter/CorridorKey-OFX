"""Model source status helper for CorridorKey model use."""


def _is_fixture_manifest(manifest):
    return (
        manifest.get("fixture") is True
        and isinstance(manifest.get("model_id"), str)
        and manifest["model_id"].startswith("corridorkey-fixture-")
        and isinstance(manifest.get("version"), str)
        and manifest["version"].endswith("-test")
        and manifest.get("backend_compatibility") == ["stub"]
        and all(
            isinstance(item, dict)
            and isinstance(item.get("path"), str)
            and item["path"].endswith(".fixture")
            for item in manifest.get("expected_files", [])
        )
        and manifest.get("local_path", "") in ("", None)
    )


class ModelSourceGate:
    """Represents local model-source readiness.

    The default local mode accepts validated local manifests. Tests can opt
    into the stricter fixture-only mode when exercising fixture bypass behavior.
    """

    def __init__(self, usage_mode="local_development", allow_local_models=True, reason=None):
        self.usage_mode = usage_mode
        self.allow_local_models = allow_local_models
        self.reason = reason or (
            "local model source is blocked by this test configuration"
        )

    @classmethod
    def local_development(cls):
        return cls()

    @classmethod
    def fixture_only(cls):
        return cls("fixture_only", allow_local_models=False)

    def status_fields(self):
        return {
            "model_source_status": "ready" if self.allow_local_models else "blocked",
            "model_source_mode": self.usage_mode,
            "model_source_blocker": "" if self.allow_local_models else self.reason,
        }

    def allows_manifest(self, manifest, fixture_allowed=False):
        return self.allow_local_models or (
            fixture_allowed and _is_fixture_manifest(manifest)
        )
