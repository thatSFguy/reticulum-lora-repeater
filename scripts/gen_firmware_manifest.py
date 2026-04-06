#!/usr/bin/env python3
"""
scripts/gen_firmware_manifest.py

Scans docs/firmware/*/ for per-release directories, each containing
one or more `reticulum-lora-repeater-<board>-<tag>.zip` files, and
emits `docs/firmware/manifest.json` describing what's available.

The webflasher fetches this manifest (same-origin, no CORS) on page
load to populate the version + board dropdowns. It replaces the
earlier GitHub Releases API call, which was blocked by
release-assets.githubusercontent.com not sending CORS headers on
the final hop of the download redirect chain.

Output schema:

  {
    "generated_at": "<ISO-8601 UTC>",
    "releases": [
      {
        "tag": "v0.1.0",
        "prerelease": false,          # true iff tag contains a '-'
        "boards": [
          {
            "name":     "Faketec",
            "zip_path": "firmware/v0.1.0/reticulum-lora-repeater-Faketec-v0.1.0.zip",
            "zip_size": 621463,
            "hex_path": "firmware/v0.1.0/reticulum-lora-repeater-Faketec-v0.1.0.hex",
            "hex_size": 1785344
          }
        ]
      }
    ]
  }

Releases are sorted newest-first by tag name (lexicographic reverse
sort on a padded version triplet). Boards within a release are
sorted alphabetically.
"""

from __future__ import annotations

import datetime as _dt
import json
import os
import re
import sys
from pathlib import Path


REPO_ROOT       = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT   = REPO_ROOT / "docs" / "firmware"
MANIFEST_PATH   = FIRMWARE_ROOT / "manifest.json"
ASSET_RE        = re.compile(
    r"^reticulum-lora-repeater-(?P<board>[^-]+)-(?P<tag>v.+)\.(?P<ext>zip|hex|uf2)$"
)


def _pad_version_for_sort(tag: str) -> tuple:
    """Best-effort version ordering key.

    Tags look like 'v0.1.0' or 'v0.1.0-test'. We want newest first,
    with prereleases sorting just below their stable counterpart.
    Returns a tuple that `sorted(reverse=True)` can order sensibly.
    """
    m = re.match(r"^v(\d+)\.(\d+)\.(\d+)(?:-(.+))?$", tag)
    if not m:
        # Unknown format — push to the end by returning zeros.
        return (0, 0, 0, "")
    major, minor, patch, pre = m.groups()
    # Empty string (stable) sorts HIGHER than any prerelease suffix
    # under normal string comparison, so stable beats prerelease at
    # the same x.y.z — which is what we want.
    return (int(major), int(minor), int(patch), pre or "~")


def scan_releases(firmware_root: Path) -> list[dict]:
    if not firmware_root.is_dir():
        return []

    releases: list[dict] = []
    for entry in sorted(firmware_root.iterdir()):
        if not entry.is_dir():
            continue
        tag = entry.name
        # Group assets in this release by board name so each board's
        # .zip and .hex end up on the same record.
        by_board: dict[str, dict] = {}
        for asset in sorted(entry.iterdir()):
            if not asset.is_file():
                continue
            m = ASSET_RE.match(asset.name)
            if not m:
                continue
            board = m.group("board")
            # Sanity check: the tag embedded in the filename should
            # match the directory name. If it doesn't, skip the file
            # and log a warning — probably a stale manual copy.
            if m.group("tag") != tag:
                print(
                    f"warn: {asset} tag mismatch (expected {tag}, "
                    f"got {m.group('tag')}) — skipping",
                    file=sys.stderr,
                )
                continue
            rec = by_board.setdefault(board, {"name": board})
            ext = m.group("ext")
            rec[f"{ext}_path"] = f"firmware/{tag}/{asset.name}"
            rec[f"{ext}_size"] = asset.stat().st_size

        if not by_board:
            continue

        releases.append({
            "tag":        tag,
            "prerelease": "-" in tag,
            "boards":     sorted(by_board.values(), key=lambda b: b["name"]),
        })

    # Newest version first.
    releases.sort(key=lambda r: _pad_version_for_sort(r["tag"]), reverse=True)
    return releases


def main() -> int:
    FIRMWARE_ROOT.mkdir(parents=True, exist_ok=True)
    releases = scan_releases(FIRMWARE_ROOT)
    manifest = {
        "generated_at": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "releases":     releases,
    }
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {MANIFEST_PATH} — {len(releases)} release(s)")
    for r in releases:
        boards = ", ".join(b["name"] for b in r["boards"])
        print(f"  {r['tag']}  ({boards}){'  [prerelease]' if r['prerelease'] else ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
