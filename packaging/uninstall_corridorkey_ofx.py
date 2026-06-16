#!/usr/bin/env python3
"""Uninstall CorridorKey OFX plugin/runtime files.

By default this removes only install artifacts: the OpenFX bundle and any
separate sidecar install directory passed to the script. User model/log data is
preserved unless --purge-user-data is explicitly supplied.
"""

from __future__ import annotations

import argparse
import json
import plistlib
import platform
import shutil
import sys
from pathlib import Path


PLUGIN_BUNDLE_NAME = "CorridorKey.ofx.bundle"
APP_NAME = "CorridorKey OFX"
PLUGIN_IDENTIFIER = "com.corridorkey.openfx"
USER_DATA_SENTINEL = ".corridorkey-ofx-user-data"


def _default_plugin_bundle() -> Path:
    if platform.system() != "Darwin":
        raise RuntimeError("CorridorKey Apple 1.0 uninstaller requires macOS")
    return Path("/Library/OFX/Plugins") / PLUGIN_BUNDLE_NAME


def _default_sidecar_dir() -> Path:
    if platform.system() != "Darwin":
        raise RuntimeError("CorridorKey Apple 1.0 uninstaller requires macOS")
    return Path("/Library/Application Support") / APP_NAME / "sidecar"


def _default_user_data_root() -> Path:
    return Path.home() / ".corridorkey-ofx"


def _is_corridorkey_bundle(path: Path) -> bool:
    if path.name != PLUGIN_BUNDLE_NAME:
        return False
    if not path.exists():
        return True
    info_plist = path / "Contents" / "Info.plist"
    try:
        with info_plist.open("rb") as handle:
            info = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException):
        return False
    return info.get("CFBundleIdentifier") == PLUGIN_IDENTIFIER


def _is_corridorkey_sidecar_dir(path: Path) -> bool:
    if path.name != "sidecar":
        return False
    if not path.exists():
        return True
    return (path / "corridorkey_sidecar" / "server.py").is_file()


def _same_path(left: Path, right: Path) -> bool:
    return left.expanduser().resolve(strict=False) == right.expanduser().resolve(strict=False)


def _is_corridorkey_user_data_root(path: Path) -> bool:
    path = path.expanduser()
    if _same_path(path, _default_user_data_root()):
        return True
    return path.name in (".corridorkey-ofx", APP_NAME) and (path / USER_DATA_SENTINEL).is_file()


def _remove_path(path: Path, dry_run: bool) -> dict[str, str]:
    path = path.expanduser()
    if not path.exists() and not path.is_symlink():
        return {"path": str(path), "action": "absent"}
    if dry_run:
        return {"path": str(path), "action": "would_remove"}
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()
    return {"path": str(path), "action": "removed"}


def _purge_user_data(root: Path, dry_run: bool) -> list[dict[str, str]]:
    root = root.expanduser()
    if not _is_corridorkey_user_data_root(root):
        raise ValueError(f"refusing to purge non-CorridorKey user data root: {root}")
    actions: list[dict[str, str]] = []
    for name in ("models", "logs", "cache", "support-bundles"):
        actions.append(_remove_path(root / name, dry_run))
    if root.exists() and root.is_dir():
        try:
            has_entries = any(root.iterdir())
        except OSError:
            has_entries = True
        if not has_entries:
            actions.append(_remove_path(root, dry_run))
        else:
            actions.append({"path": str(root), "action": "preserved_nonempty_user_root"})
    else:
        actions.append({"path": str(root), "action": "absent"})
    return actions


def uninstall(args: argparse.Namespace) -> dict[str, object]:
    if not _is_corridorkey_bundle(args.plugin_bundle.expanduser()):
        raise ValueError(f"refusing to remove non-CorridorKey plugin bundle: {args.plugin_bundle}")
    if not _is_corridorkey_sidecar_dir(args.sidecar_install_dir.expanduser()):
        raise ValueError(
            f"refusing to remove non-CorridorKey sidecar directory: {args.sidecar_install_dir}"
        )
    if args.purge_user_data and not _is_corridorkey_user_data_root(
        args.user_data_root.expanduser()
    ):
        raise ValueError(f"refusing to purge non-CorridorKey user data root: {args.user_data_root}")

    actions: list[dict[str, str]] = []
    actions.append(_remove_path(args.plugin_bundle, args.dry_run))
    actions.append(_remove_path(args.sidecar_install_dir, args.dry_run))

    if args.purge_user_data:
        actions.extend(_purge_user_data(args.user_data_root, args.dry_run))
    else:
        actions.append(
            {
                "path": str(args.user_data_root.expanduser()),
                "action": "preserved_user_data_default",
            }
        )

    return {
        "ok": True,
        "dry_run": args.dry_run,
        "purge_user_data": args.purge_user_data,
        "actions": actions,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--plugin-bundle",
        type=Path,
        default=_default_plugin_bundle(),
        help="CorridorKey.ofx.bundle path to remove",
    )
    parser.add_argument(
        "--sidecar-install-dir",
        type=Path,
        default=_default_sidecar_dir(),
        help="Separate installed sidecar runtime directory to remove when present",
    )
    parser.add_argument(
        "--user-data-root",
        type=Path,
        default=_default_user_data_root(),
        help="User data root containing models/logs/cache; preserved unless --purge-user-data",
    )
    parser.add_argument(
        "--purge-user-data",
        action="store_true",
        help="Opt-in destructive purge of user models, logs, cache, and support bundles",
    )
    parser.add_argument("--dry-run", action="store_true", help="Report actions without removing files")
    parser.add_argument("--yes", action="store_true", help="Confirm removal without interactive prompt")
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON output")
    args = parser.parse_args(argv)
    if not args.dry_run and not args.yes:
        parser.error("refusing to remove files without --yes; use --dry-run to inspect first")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        result = uninstall(args)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        for action in result["actions"]:
            print(f"{action['action']}: {action['path']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
