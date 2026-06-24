#!/usr/bin/env python3
"""
scripts/mk_dfu_json.py — turn a PlatformIO / adafruit-nrfutil DFU package
(firmware.zip) into a single self-contained JSON the web flasher streams
directly to the bootloader.

The webflasher used to fetch the firmware.zip and unzip it in the browser
with JSZip. That works, but it ships a DEFLATE implementation to every
visitor and parses a binary container client-side. Instead, CI pre-extracts
the two blobs the serial DFU protocol actually needs — the application
binary and its init/"dat" packet — into one JSON file the browser can fetch
and feed straight into docs/dfu.js without any unzip step.

    python3 scripts/mk_dfu_json.py <firmware.zip> <out.dfu.json>

Output schema (consumed by DfuPackage.fromDfuJson in docs/dfu.js):

    {
      "fw_bin_b64":   "<base64 of application.bin>",
      "init_dat_hex": "<hex of application.dat>",
      "app_size":     <len(application.bin)>,
      "dfu_version":  "<manifest dfu_version, informational>"
    }
"""
import sys
import json
import base64
import zipfile


def make(zip_path: str) -> dict:
    z = zipfile.ZipFile(zip_path)
    manifest = json.loads(z.read("manifest.json"))
    app = manifest["manifest"]["application"]
    bin_bytes = z.read(app["bin_file"])
    dat_bytes = z.read(app["dat_file"])
    return {
        "fw_bin_b64": base64.b64encode(bin_bytes).decode("ascii"),
        "init_dat_hex": dat_bytes.hex(),
        "app_size": len(bin_bytes),
        "dfu_version": manifest["manifest"].get("dfu_version"),
    }


def main(zip_path: str, out_path: str) -> int:
    obj = make(zip_path)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(obj, f, separators=(",", ":"))
    print(
        f"{out_path}: app={obj['app_size']}B "
        f"init={len(obj['init_dat_hex']) // 2}B"
    )
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: mk_dfu_json.py <firmware.zip> <out.dfu.json>", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1], sys.argv[2]))
