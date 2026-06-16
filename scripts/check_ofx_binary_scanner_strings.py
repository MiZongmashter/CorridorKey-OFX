#!/usr/bin/env python3
"""Report scanner-sensitive substrings in a built CorridorKey OFX binary."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

CATEGORIES = {
    "runtime_dependency_terms": ["python", "torch", "mlx", "mps"],
    "product_terms": ["sidecar", "model"],
    "loader_terms": ["DYLD", "LD_LIBRARY", "PYTHONNOUSERSITE", "exec", "fork"],
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="Path to built .ofx binary")
    parser.add_argument(
        "--fail-on-findings",
        action="store_true",
        help="Exit non-zero if any configured term is found",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.binary.is_file():
        print(f"error: binary does not exist: {args.binary}", file=sys.stderr)
        return 2

    strings = shutil.which("strings")
    if strings is None:
        print("report unavailable: required command not found: strings", file=sys.stderr)
        return 2 if args.fail_on_findings else 77

    completed = subprocess.run(
        [strings, str(args.binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        print(completed.stderr.decode(errors="replace"), file=sys.stderr, end="")
        return completed.returncode

    haystack = completed.stdout.decode(errors="replace").splitlines()
    found_any = False
    print(f"OFX binary scanner string report: {args.binary}")
    for category, terms in CATEGORIES.items():
        matches: list[str] = []
        for line in haystack:
            lowered = line.lower()
            if any(term.lower() in lowered for term in terms):
                matches.append(line)
        if matches:
            found_any = True
            print(f"\n[{category}]")
            for match in sorted(set(matches)):
                print(match)

    if not found_any:
        print("No configured scanner-sensitive strings found.")
    elif args.fail_on_findings:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
