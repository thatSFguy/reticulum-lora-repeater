#
# reticulum-lora-repeater / scripts/pre_build.py
# -------------------------------------------------------------------
# PlatformIO pre-build script. Jobs:
#
#   0. Stamp RLR_VERSION from `git describe`.
#   1. Strip --specs=nano.specs from LINKFLAGS so C++ exceptions work.
#   2. Patch microReticulum to add DATA/PROOF forwarding for transport
#      (repeater) nodes — upstream still drops transit packets whose
#      destination isn't a local destination.
#   3. Create a Bluefruit ble.h shim for SoftDevice header resolution.
#
# microReticulum / microStore are pinned to specific commits in
# platformio.ini, so the source these patches target is reproducible.
#
# ---- A note on patches that USED to live here ----
#
#   Earlier revisions of this script also patched microReticulum for:
#     * Reticulum 0.7+ ratchet announce validation, and
#     * skipping redundant path-table flash writes.
#   The pinned microReticulum (attermann/microReticulum@5fbdbf3) now
#   implements BOTH natively — ratchet parsing in Identity.cpp
#   (context_flag branch) and CRC-gated path-table writes in
#   Transport::write_path_table — so those patches were removed as
#   redundant. See git history if you need the originals.
#
#   ---- Job 1: --specs=nano.specs ----
#
#   The nordicnrf52 PlatformIO platform adds --specs=nano.specs to
#   LINKFLAGS programmatically as part of its SConstruct setup. This
#   happens AFTER build_unflags in platformio.ini is processed, so
#   putting `--specs=nano.specs` in build_unflags does NOT actually
#   remove it from LINKFLAGS at link time — it gets silently re-added.
#
#   newlib-nano's libc does not support C++ exceptions correctly. When
#   a library linked with nano.specs throws anything, the program
#   crashes or hangs silently — typically before USB CDC has flushed
#   any Serial.print output. The microReticulum library throws
#   std::runtime_error from several legitimate error paths, so without
#   this fix the firmware boots in complete silence on any unexpected
#   runtime error.
#

import os
import subprocess

Import("env")  # noqa: F821  (PlatformIO injects this)

# ---------------------------------------------------------------
#  Job 0 — stamp firmware version from git
# ---------------------------------------------------------------
#
#  Derive RLR_VERSION from `git describe --tags --always` so the
#  boot banner and VERSION serial command report the actual build
#  version (e.g. "v0.1.4" on a clean tag, "v0.1.4-3-gabcdef0" on
#  a commit past the tag). Falls back to "dev" if git isn't
#  available or the repo has no tags.

try:
    git_version = subprocess.check_output(
        ["git", "describe", "--tags", "--always"],
        stderr=subprocess.DEVNULL,
    ).decode("utf-8").strip()
except Exception:
    git_version = "dev"

env.Append(CPPDEFINES=[("RLR_VERSION", env.StringifyMacro(git_version))])
print("pre_build: RLR_VERSION = {}".format(git_version))

# ---------------------------------------------------------------
#  Job 1 — C++ exceptions linker fix
# ---------------------------------------------------------------

platform = env.GetProjectOption("platform")  # noqa: F821
if platform == "nordicnrf52":
    if "--specs=nano.specs" in env["LINKFLAGS"]:  # noqa: F821
        env["LINKFLAGS"].remove("--specs=nano.specs")  # noqa: F821
        print("pre_build: removed --specs=nano.specs from LINKFLAGS (C++ exceptions fix)")
    else:
        print("pre_build: --specs=nano.specs not found in LINKFLAGS (may be already removed)")


def _microreticulum_src(env, filename):
    """Absolute path to a source file inside the fetched microReticulum
    library. Headers/sources live under src/microReticulum/ (the library
    moved everything behind that prefix; the umbrella header is
    src/microReticulum.h)."""
    return os.path.join(
        env["PROJECT_DIR"], ".pio", "libdeps", env["PIOENV"],
        "microReticulum", "src", "microReticulum", filename,
    )


# ---------------------------------------------------------------
#  Job 2 — DATA / PROOF packet forwarding for transport nodes
#
#  microReticulum's Transport::inbound() handles ANNOUNCE rebroadcast
#  correctly but drops DATA packets that aren't for a local destination.
#  Upstream Python Reticulum forwards these packets when transport is
#  enabled. For a single-interface repeater, we just rebroadcast the
#  raw packet with an incremented hop count.
#
#  This patch adds forwarding logic to the "Local destination not found"
#  branch (DATA) and the "Proof is not candidate for transporting"
#  branch (PROOF) in Transport.cpp.
# ---------------------------------------------------------------

DATA_FWD_MARKER = "// RLR_DATA_FORWARD_PATCH"

DATA_FWD_ORIGINAL = '\t\t\t\telse {\n\t\t\t\t\tDEBUGF("Transport::inbound: Local destination %s not found, not handling packet locally", packet.destination_hash().toHex().c_str());\n\t\t\t\t}'

DATA_FWD_PATCHED = """\t\t\t\telse {
\t\t\t\t\t""" + DATA_FWD_MARKER + """
\t\t\t\t\tDEBUGF("Transport::inbound: Local destination %s not found, not handling packet locally", packet.destination_hash().toHex().c_str());
\t\t\t\t\t// Forward DATA packets when transport is enabled (repeater mode).
\t\t\t\t\t// Rebroadcast with updated hop count on all interfaces.
\t\t\t\t\tif (Reticulum::transport_enabled() && packet.hops() < Type::Transport::PATHFINDER_M) {
\t\t\t\t\t\tBytes new_raw(packet.raw().size());
\t\t\t\t\t\tnew_raw << packet.raw().left(1);      // flags byte
\t\t\t\t\t\tnew_raw << packet.hops();              // updated hop count
\t\t\t\t\t\tnew_raw << packet.raw().mid(2);         // rest of packet unchanged
\t\t\t\t\t\tfor (auto& [hash, iface] : _interfaces) {
\t\t\t\t\t\t\tDEBUGF("Transport::inbound: Forwarding DATA packet for %s on %s (hops=%d)", packet.destination_hash().toHex().c_str(), iface.toString().c_str(), packet.hops());
\t\t\t\t\t\t\ttransmit(iface, new_raw);
\t\t\t\t\t\t}
\t\t\t\t\t}
\t\t\t\t}"""

# Also patch PROOF forwarding — when transport is enabled and proof
# is not in reverse_table, rebroadcast it so proofs can traverse
# the repeater back to the sender.
PROOF_FWD_MARKER = "// RLR_PROOF_FORWARD_PATCH"

PROOF_FWD_ORIGINAL = '\t\t\t\t\tTRACE("Proof is not candidate for transporting");'

PROOF_FWD_PATCHED = """\t\t\t\t\t""" + PROOF_FWD_MARKER + """
\t\t\t\t\t// Forward PROOFs when transport is enabled (repeater mode).
\t\t\t\t\tif (Reticulum::transport_enabled() && packet.hops() < Type::Transport::PATHFINDER_M) {
\t\t\t\t\t\tBytes new_raw(packet.raw().size());
\t\t\t\t\t\tnew_raw << packet.raw().left(1);
\t\t\t\t\t\tnew_raw << packet.hops();
\t\t\t\t\t\tnew_raw << packet.raw().mid(2);
\t\t\t\t\t\tfor (auto& [hash, iface] : _interfaces) {
\t\t\t\t\t\t\tDEBUGF("Transport::inbound: Forwarding PROOF on %s (hops=%d)", iface.toString().c_str(), packet.hops());
\t\t\t\t\t\t\ttransmit(iface, new_raw);
\t\t\t\t\t\t}
\t\t\t\t\t}
\t\t\t\t\telse {
\t\t\t\t\t\tTRACE("Proof is not candidate for transporting");
\t\t\t\t\t}"""


def patch_data_forwarding(env):
    target = _microreticulum_src(env, "Transport.cpp")
    if not os.path.exists(target):
        print("pre_build: microReticulum not yet fetched — data forwarding patch skipped")
        return

    with open(target, "r", encoding="utf-8", newline="") as f:
        source = f.read()
    source = source.replace("\r\n", "\n")

    applied = False

    # Patch 1: DATA forwarding
    if DATA_FWD_MARKER in source:
        print("pre_build: DATA forwarding patch already applied")
    elif DATA_FWD_ORIGINAL not in source:
        raise RuntimeError(
            "pre_build: could not locate DATA handling block in Transport.cpp "
            "for data forwarding patch. The upstream source may have changed — "
            "re-mine the block and update DATA_FWD_ORIGINAL in scripts/pre_build.py."
        )
    else:
        source = source.replace(DATA_FWD_ORIGINAL, DATA_FWD_PATCHED, 1)
        assert DATA_FWD_MARKER in source
        print("pre_build: applied DATA forwarding patch")
        applied = True

    # Patch 2: PROOF forwarding
    if PROOF_FWD_MARKER in source:
        print("pre_build: PROOF forwarding patch already applied")
    elif PROOF_FWD_ORIGINAL not in source:
        print("pre_build: PROOF forwarding target not found (may have changed upstream)")
    else:
        source = source.replace(PROOF_FWD_ORIGINAL, PROOF_FWD_PATCHED, 1)
        assert PROOF_FWD_MARKER in source
        print("pre_build: applied PROOF forwarding patch")
        applied = True

    if applied:
        with open(target, "w", encoding="utf-8", newline="") as f:
            f.write(source)


# ---------------------------------------------------------------
#  Job 3 — Bluefruit ble.h shim for SoftDevice header resolution
#
#  The Adafruit Bluefruit52Lib's bluefruit_common.h does
#  #include "ble.h" (quotes). On some PlatformIO setups the
#  compiler's quote-include search fails to reach the SoftDevice
#  directory even though it's on the -I path. Creating a tiny
#  forwarding shim directly in the Bluefruit src/ directory that
#  uses an explicit relative path to the real SoftDevice ble.h
#  fixes the resolution.
# ---------------------------------------------------------------

def patch_bluefruit_ble_shim(env):
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")
    if not framework_dir:
        return

    bluefruit_src = os.path.join(framework_dir, "libraries", "Bluefruit52Lib", "src")
    shim_path = os.path.join(bluefruit_src, "ble.h")

    if os.path.exists(shim_path):
        # Already exists (from a previous run or manual creation)
        return

    # Determine the SoftDevice version from the board config
    sd_name = env.BoardConfig().get("build.softdevice.sd_name", "s140")
    sd_version = env.BoardConfig().get("build.softdevice.sd_version", "6.1.1")
    sd_api_dir = "%s_nrf52_%s_API" % (sd_name, sd_version)

    shim_content = (
        "// Auto-generated by pre_build.py — Bluefruit ble.h shim\n"
        "// Forwards to the real SoftDevice ble.h via explicit relative path.\n"
        '#include "../../cores/nRF5/nordic/softdevice/%s/include/ble.h"\n'
        % sd_api_dir
    )

    with open(shim_path, "w") as f:
        f.write(shim_content)
    print("pre_build: created Bluefruit ble.h shim -> %s" % sd_api_dir)


patch_bluefruit_ble_shim(env)     # noqa: F821
patch_data_forwarding(env)        # noqa: F821


# ---------------------------------------------------------------
#  Install custom linker script for XIAO nRF52840 (S140 v7)
# ---------------------------------------------------------------
# The custom board JSON references nrf52840_s140_v7.ld by name.
# PlatformIO looks for it in the BSP's linker directory. Copy our
# version there if it doesn't exist yet (needed on CI where the
# local copy from the dev machine isn't present).
_bsp_linker_dir = os.path.join(
    env.subst("$PROJECT_PACKAGES_DIR"),  # noqa: F821
    "framework-arduinoadafruitnrf52", "cores", "nRF5", "linker"
)
_v7_ld_src = os.path.join(env.subst("$PROJECT_DIR"), "linker", "nrf52840_s140_v7.ld")  # noqa: F821
_v7_ld_dst = os.path.join(_bsp_linker_dir, "nrf52840_s140_v7.ld")
if os.path.exists(_v7_ld_src) and not os.path.exists(_v7_ld_dst):
    import shutil
    os.makedirs(_bsp_linker_dir, exist_ok=True)
    shutil.copy2(_v7_ld_src, _v7_ld_dst)
    print("pre_build: installed nrf52840_s140_v7.ld to BSP linker directory")

# Ensure scripts/ is in sys.path so hex2uf2 can be found on CI.
# PlatformIO extra_scripts are exec()'d, not imported, so __file__
# is not defined. Use the project dir from the env instead.
import sys as _sys
_scripts_dir = os.path.join(env.subst("$PROJECT_DIR"), "scripts")  # noqa: F821
if _scripts_dir not in _sys.path:
    _sys.path.insert(0, _scripts_dir)
import hex2uf2 as _hex2uf2  # noqa: E402

def _generate_uf2(source, target, env):
    firmware_dir = env.subst("$BUILD_DIR")
    hex_path = os.path.join(firmware_dir, "firmware.hex")
    uf2_path = os.path.join(firmware_dir, "firmware.uf2")
    if os.path.exists(hex_path):
        _hex2uf2.convert_to_uf2(hex_path, uf2_path)

env.AddPostAction("buildprog", _generate_uf2)  # noqa: F821
