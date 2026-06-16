#!/usr/bin/env python3
"""Fetch or vendor the official OpenFX SDK into third_party/openfx."""

from __future__ import annotations

import datetime as _dt
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SDK_URL = "https://github.com/AcademySoftwareFoundation/openfx.git"
SDK_COMMIT = "cf6cbf978e02475a52ff9a85973c8d8146f5bd23"
SDK_ROOT = REPO_ROOT / "third_party" / "openfx"
SDK_DOC = REPO_ROOT / "third_party" / "OPENFX_SDK.md"
RISK_DOC = REPO_ROOT / "docs" / "qa" / "openfx-sdk-unavailable-risk.md"
EXPECTED_HEADERS = (
    "include/ofxCore.h",
    "include/ofxImageEffect.h",
    "include/ofxParam.h",
    "include/ofxProperty.h",
    "Support/include/ofxsImageEffect.h",
)
PROVENANCE_BEGIN = "<!-- BEGIN OPENFX SDK PROVENANCE -->"
PROVENANCE_END = "<!-- END OPENFX SDK PROVENANCE -->"


def _run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def _missing_headers(root: Path) -> list[str]:
    return [header for header in EXPECTED_HEADERS if not (root / header).is_file()]


def _git_commit(root: Path) -> str | None:
    completed = _run(["git", "-C", str(root), "rev-parse", "HEAD"])
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def _git_remote(root: Path) -> str | None:
    completed = _run(["git", "-C", str(root), "remote", "get-url", "origin"])
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def _redact_text(text: str) -> str:
    replacements = {
        str(REPO_ROOT): "<repo>",
        str(SDK_ROOT): "third_party/openfx",
    }
    source = os.environ.get("OPENFX_SDK_SOURCE_DIR")
    if source:
        replacements[str(Path(source).expanduser())] = "<OPENFX_SDK_SOURCE_DIR>"

    redacted = text
    for raw_value, replacement in sorted(replacements.items(), key=lambda item: len(item[0]), reverse=True):
        if raw_value:
            redacted = redacted.replace(raw_value, replacement)
    return redacted


def _header_tree_checksum(root: Path) -> str:
    digest = hashlib.sha256()
    headers: list[Path] = []
    for header_root in (root / "include", root / "Support" / "include"):
        if header_root.is_dir():
            headers.extend(path for path in header_root.rglob("*.h") if path.is_file())

    for path in sorted(headers, key=lambda item: item.relative_to(root).as_posix()):
        rel = path.relative_to(root).as_posix()
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return f"sha256:{digest.hexdigest()}"


def _source_label(source: Path) -> str:
    explicit_label = os.environ.get("OPENFX_SDK_SOURCE_LABEL")
    if explicit_label:
        return explicit_label
    label_digest = hashlib.sha256(str(source.resolve()).encode("utf-8")).hexdigest()[:12]
    return f"{source.name or 'openfx-source'}#{label_digest}"


def _replace_provenance_block(new_block: str) -> None:
    existing = SDK_DOC.read_text(encoding="utf-8") if SDK_DOC.exists() else ""
    block = f"{PROVENANCE_BEGIN}\n{new_block.rstrip()}\n{PROVENANCE_END}"
    start = existing.find(PROVENANCE_BEGIN)
    end = existing.rfind(PROVENANCE_END)
    if start != -1 and end != -1 and end > start:
        before = existing[:start]
        after = existing[end + len(PROVENANCE_END):]
        updated = before.rstrip() + "\n\n" + block + after
    else:
        updated = existing.rstrip() + "\n\n## SDK Provenance\n\n" + block + "\n"
    SDK_DOC.write_text(updated, encoding="utf-8")


def _record_provenance(acquisition: str, root: Path, source: Path | None = None) -> None:
    commit = _git_commit(root)
    remote = _git_remote(root)
    source_version = _git_commit(source) if source else None
    lines = [
        "Status: available",
        f"Acquisition: {acquisition}",
        f"Recorded: {_dt.datetime.now(_dt.timezone.utc).isoformat()}",
        "SDK root: third_party/openfx",
        f"Expected git commit: {SDK_COMMIT}",
        f"Header tree checksum: {_header_tree_checksum(root)}",
    ]
    lines.append(f"Git remote: {remote or (SDK_URL if commit else 'not available')}")
    if commit:
        lines.append(f"Git commit: {commit}")
    if source:
        lines.append(f"Source label: {_source_label(source)}")
        lines.append(f"Source version: {source_version or 'not available'}")
    _replace_provenance_block("\n".join(lines))
    if RISK_DOC.exists():
        RISK_DOC.unlink()


def _write_unavailable_risk(attempted_command: str, reason: str) -> None:
    RISK_DOC.parent.mkdir(parents=True, exist_ok=True)
    source_state = "OPENFX_SDK_SOURCE_DIR was not set"
    if os.environ.get("OPENFX_SDK_SOURCE_DIR"):
        source_state = "OPENFX_SDK_SOURCE_DIR did not point to an official SDK layout"
    redacted_reason = _redact_text(reason)
    content = f"""# OpenFX SDK Unavailable Risk

Date: {_dt.date.today().isoformat()}

## Attempted Command

```text
{attempted_command}
```

## Missing Source

{source_state}. The required official SDK source is `{SDK_URL}` or an already-downloaded copy with the expected headers.

## Failure

```text
{redacted_reason.strip() or 'No command output was captured.'}
```

## Why Sessions 2+ Are Blocked

Sessions 2+ require the official OpenFX SDK and support headers to compile a scanner-safe plugin. Without a fetched or supplied SDK at `third_party/openfx`, CMake discovery must fail and no OpenFX plugin binary or bundle can be validated.
"""
    RISK_DOC.write_text(content, encoding="utf-8")


def _copy_source(source: Path) -> int:
    if not source.exists() or not source.is_dir():
        _write_unavailable_risk(
            "copy OPENFX_SDK_SOURCE_DIR third_party/openfx",
            "OPENFX_SDK_SOURCE_DIR does not exist or is not a directory.",
        )
        return 1
    missing_headers = _missing_headers(source)
    if missing_headers:
        missing = "\n".join(missing_headers)
        _write_unavailable_risk(
            "copy OPENFX_SDK_SOURCE_DIR third_party/openfx",
            f"OPENFX_SDK_SOURCE_DIR is missing expected headers:\n{missing}",
        )
        return 1

    SDK_ROOT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        source,
        SDK_ROOT,
        ignore=shutil.ignore_patterns(".git", "build", "Build", "__pycache__"),
    )
    _record_provenance("OPENFX_SDK_SOURCE_DIR copy", SDK_ROOT, source)
    print("OpenFX SDK copied into third_party/openfx")
    return 0


def _clone_sdk() -> int:
    attempted = f"git init third_party/openfx && git fetch --depth 1 {SDK_URL} {SDK_COMMIT}"
    SDK_ROOT.parent.mkdir(parents=True, exist_ok=True)
    SDK_ROOT.mkdir(parents=True, exist_ok=True)
    commands = (
        ["git", "init"],
        ["git", "remote", "add", "origin", SDK_URL],
        ["git", "fetch", "--depth", "1", "origin", SDK_COMMIT],
        ["git", "checkout", "--detach", SDK_COMMIT],
    )
    output = []
    failed_returncode = 0
    for command in commands:
        completed = _run(command, cwd=SDK_ROOT)
        output.append(completed.stdout)
        if completed.returncode != 0:
            failed_returncode = completed.returncode
            break

    if failed_returncode:
        if SDK_ROOT.exists():
            shutil.rmtree(SDK_ROOT)
        _write_unavailable_risk(attempted, "".join(output))
        print(f"OpenFX SDK fetch failed; wrote {RISK_DOC.relative_to(REPO_ROOT)}", file=sys.stderr)
        return failed_returncode

    missing_headers = _missing_headers(SDK_ROOT)
    if missing_headers:
        missing = "\n".join(missing_headers)
        _write_unavailable_risk(attempted, f"Fetched SDK is missing expected headers:\n{missing}")
        return 1

    _record_provenance(f"git fetch --depth 1 {SDK_COMMIT}", SDK_ROOT)
    print("OpenFX SDK fetched into third_party/openfx")
    return 0


def main() -> int:
    if SDK_ROOT.exists():
        missing_headers = _missing_headers(SDK_ROOT)
        if not missing_headers:
            commit = _git_commit(SDK_ROOT)
            if (SDK_ROOT / ".git").exists() and commit and commit != SDK_COMMIT:
                print(
                    "third_party/openfx is a git checkout with expected headers, but it does not "
                    f"match the pinned OpenFX SDK commit {SDK_COMMIT}.\n"
                    "Remove third_party/openfx and rerun this script, or supply an official copied "
                    "SDK through OPENFX_SDK_SOURCE_DIR.",
                    file=sys.stderr,
                )
                return 1
            acquisition = "existing git checkout" if (SDK_ROOT / ".git").exists() else "existing copied SDK"
            _record_provenance(acquisition, SDK_ROOT)
            print("OpenFX SDK already present at third_party/openfx")
            return 0

        missing = "\n  ".join(missing_headers)
        print(
            "third_party/openfx exists but is not an official OpenFX SDK layout.\n"
            "Remove or replace it with https://github.com/AcademySoftwareFoundation/openfx.git.\n"
            f"Missing:\n  {missing}",
            file=sys.stderr,
        )
        return 1

    source = os.environ.get("OPENFX_SDK_SOURCE_DIR")
    if source:
        return _copy_source(Path(source).expanduser())

    return _clone_sdk()


if __name__ == "__main__":
    raise SystemExit(main())
