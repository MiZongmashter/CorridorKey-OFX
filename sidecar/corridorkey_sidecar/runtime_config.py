"""Runtime path configuration for optional CorridorKey backends."""

import os
from dataclasses import dataclass
from pathlib import Path


DEFAULT_CORRIDORKEY_REPO = Path("external") / "CorridorKey"
DEFAULT_MODEL_DIR = Path(".local") / "models" / "corridorkey"
DEFAULT_BACKEND_FIXTURE_DIR = Path("tests") / "fixtures" / "backend" / "private"
BACKEND_FIXTURE_MARKER = ".corridorkey-backend-fixture"


@dataclass(frozen=True)
class RuntimePathIssue:
    env_name: str
    path: Path
    message: str


@dataclass(frozen=True)
class RuntimeConfig:
    corridorkey_repo: Path
    model_dir: Path
    backend_fixture_dir: Path
    device: str

    @classmethod
    def from_env(cls, cwd=None):
        base = Path.cwd() if cwd is None else Path(cwd)
        return cls(
            corridorkey_repo=_path_from_env(
                "CORRIDORKEY_REPO", base / DEFAULT_CORRIDORKEY_REPO
            ),
            model_dir=_path_from_env("CORRIDORKEY_MODEL_DIR", base / DEFAULT_MODEL_DIR),
            backend_fixture_dir=_path_from_env(
                "CORRIDORKEY_BACKEND_FIXTURE_DIR",
                base / DEFAULT_BACKEND_FIXTURE_DIR,
            ),
            device=os.environ.get("CORRIDORKEY_DEVICE", "cpu") or "cpu",
        )

    def required_path_issues(self):
        checks = (
            ("CORRIDORKEY_REPO", self.corridorkey_repo),
            ("CORRIDORKEY_MODEL_DIR", self.model_dir),
        )
        return self._path_issues(checks)

    def required_fixture_path_issues(self):
        checks = (
            ("CORRIDORKEY_REPO", self.corridorkey_repo),
            ("CORRIDORKEY_MODEL_DIR", self.model_dir),
            ("CORRIDORKEY_BACKEND_FIXTURE_DIR", self.backend_fixture_dir),
        )
        return self._path_issues(checks)

    def _path_issues(self, checks):
        issues = []
        for env_name, path in checks:
            if not path.is_dir():
                issues.append(
                    RuntimePathIssue(
                        env_name=env_name,
                        path=path,
                        message=f"{env_name} must point to an existing directory",
                    )
                )
        return issues

    def fixture_runtime_enabled(self):
        marker = self.backend_fixture_dir / BACKEND_FIXTURE_MARKER
        try:
            return marker.read_text(encoding="utf-8").strip() == "fixture-only"
        except OSError:
            return False

    def status_fields(self):
        return {
            "corridorkey_repo_configured": _bool_text(self.corridorkey_repo.is_dir()),
            "model_dir_configured": _bool_text(self.model_dir.is_dir()),
            "backend_fixture_dir_configured": _bool_text(
                self.backend_fixture_dir.is_dir()
            ),
            "corridorkey_device": self.device,
            "fixture_runtime": _bool_text(self.fixture_runtime_enabled()),
        }


def _path_from_env(env_name, default_path):
    value = os.environ.get(env_name)
    if value:
        return Path(value).expanduser()
    return Path(default_path).expanduser()


def _bool_text(value):
    return "true" if value else "false"
