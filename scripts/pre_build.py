#
# reticulum-lora-repeater / scripts/pre_build.py
# -------------------------------------------------------------------
# PlatformIO pre-build script. Two jobs:
#
#   1. Strip --specs=nano.specs from LINKFLAGS so C++ exceptions work.
#   2. Patch microReticulum's Identity::validate_announce() in place
#      inside the PlatformIO libdeps cache to teach it the Reticulum
#      0.7+ ratchet announce wire format. See docs/RATCHET_PROTOCOL.md
#      for the full spec and justification.
#
# ---- Job 1: --specs=nano.specs ----
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
# ---- Job 2: ratchet announce patch ----
#
#   Upstream attermann/microReticulum was last synced against a
#   pre-0.7 Reticulum, before ratchet forward-secrecy landed.
#   Sideband, NomadNet, reticulum-meshchat, and current RNode
#   firmware all emit announces with a 32-byte ratchet key inserted
#   between random_hash and signature, signalled by context_flag
#   (bit 5 of the packet's first header byte). microReticulum's
#   Packet.cpp ALREADY packs/unpacks context_flag correctly — the
#   plumbing is there, the author just never wired it through to
#   Identity::validate_announce, which still uses the legacy byte
#   offsets.
#
#   Result: our firmware receives Sideband announces but drops them
#   because the signature check fails on the wrong-offset signed_data.
#   Nodes stay invisible in Sideband even when our radio is fine.
#
#   Fix: a surgical replacement of the parsing + signed_data
#   construction in validate_announce, branching on packet.context_flag().
#   Legacy (context_flag = 0) path is bit-identical to the upstream
#   C++ — zero risk of regressing existing microReticulum ↔
#   microReticulum mesh traffic. Ratchet (context_flag = 1) path is
#   new and handles the 32-byte ratchet field per upstream Python's
#   validate_announce in RNS/Identity.py.
#
#   This is implemented as a pre-build string replacement against the
#   fetched lib source rather than a fork because:
#     * Zero external dependencies (no fork to maintain, no
#       `lib_deps` pin to update).
#     * Patch is reviewable in-tree alongside the rest of the repo.
#     * Idempotent — if the patch has already been applied, we
#       detect the marker and skip.
#     * Easy to upstream: the replacement text IS the upstream PR.
#
#   When attermann/microReticulum eventually merges ratchet support
#   upstream, this function becomes a no-op (the marker is already
#   present in the source) and we can delete it as cleanup.
#

import os

Import("env")  # noqa: F821  (PlatformIO injects this)

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


# ---------------------------------------------------------------
#  Job 2 — patch microReticulum Identity::validate_announce
# ---------------------------------------------------------------

# A string this patch introduces that is NOT in the upstream source.
# Used to detect whether the patch has already been applied so we
# don't re-patch on every incremental build (which would fail the
# str.replace() second time because the original text is gone).
PATCH_MARKER = "// RLR_RATCHET_PATCH"

# Exact source block from upstream microReticulum Identity.cpp
# (attermann/microReticulum@72c4ac1 and later). If upstream ever
# changes this block even slightly, str.replace() will return the
# unchanged string, the marker won't be present afterwards, and the
# assertion at the bottom of patch_microreticulum() will fail loudly
# at build time rather than silently shipping an unpatched firmware.
ORIGINAL_BLOCK = """\t\tif (packet.packet_type() == Type::Packet::ANNOUNCE) {
\t\t\tBytes destination_hash = packet.destination_hash();
\t\t\t//TRACEF(\"Identity::validate_announce: destination_hash: %s\", packet.destination_hash().toHex().c_str());
\t\t\tBytes public_key = packet.data().left(KEYSIZE/8);
\t\t\t//TRACEF(\"Identity::validate_announce: public_key:       %s\", public_key.toHex().c_str());
\t\t\tBytes name_hash = packet.data().mid(KEYSIZE/8, NAME_HASH_LENGTH/8);
\t\t\t//TRACEF(\"Identity::validate_announce: name_hash:        %s\", name_hash.toHex().c_str());
\t\t\tBytes random_hash = packet.data().mid(KEYSIZE/8 + NAME_HASH_LENGTH/8, RANDOM_HASH_LENGTH/8);
\t\t\t//TRACEF(\"Identity::validate_announce: random_hash:      %s\", random_hash.toHex().c_str());
\t\t\tBytes signature = packet.data().mid(KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8, SIGLENGTH/8);
\t\t\t//TRACEF(\"Identity::validate_announce: signature:        %s\", signature.toHex().c_str());
\t\t\tBytes app_data;
\t\t\tif (packet.data().size() > (KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8 + SIGLENGTH/8)) {
\t\t\t\tapp_data = packet.data().mid(KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8 + SIGLENGTH/8);
\t\t\t}
\t\t\t//TRACEF(\"Identity::validate_announce: app_data:         %s\", app_data.toHex().c_str());
\t\t\t//TRACEF(\"Identity::validate_announce: app_data text:    %s\", app_data.toString().c_str());

\t\t\tBytes signed_data;
\t\t\tsigned_data << packet.destination_hash() << public_key << name_hash << random_hash+app_data;
\t\t\t//TRACEF(\"Identity::validate_announce: signed_data:      %s\", signed_data.toHex().c_str());

\t\t\tif (packet.data().size() <= KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8 + SIGLENGTH/8) {
\t\t\t\tapp_data.clear();
\t\t\t}"""

# Replacement block. Adds a has_ratchet branch driven by
# packet.context_flag(). Legacy (flag = 0) path: RATCHET_BYTES is 0,
# ratchet is empty, all offsets are identical to upstream, and
# operator<< with an empty Bytes is a no-op so signed_data is
# bit-identical to upstream. Ratchet (flag = 1) path: reads 32 bytes
# between random_hash and signature, includes them in signed_data
# in the same field position upstream Python does.
PATCHED_BLOCK = """\t\tif (packet.packet_type() == Type::Packet::ANNOUNCE) {
\t\t\t""" + PATCH_MARKER + """ — Reticulum 0.7+ ratchet announce support.
\t\t\t// See docs/RATCHET_PROTOCOL.md and scripts/pre_build.py for the
\t\t\t// full story. This branch is applied by pre_build.py at build
\t\t\t// time against the upstream attermann/microReticulum source.
\t\t\tBytes destination_hash = packet.destination_hash();
\t\t\tconst bool has_ratchet = (packet.context_flag() == Type::Packet::FLAG_SET);
\t\t\tconst size_t RATCHET_BYTES = has_ratchet ? (RATCHETSIZE/8) : 0;
\t\t\tBytes public_key = packet.data().left(KEYSIZE/8);
\t\t\tBytes name_hash = packet.data().mid(KEYSIZE/8, NAME_HASH_LENGTH/8);
\t\t\tBytes random_hash = packet.data().mid(KEYSIZE/8 + NAME_HASH_LENGTH/8, RANDOM_HASH_LENGTH/8);
\t\t\tBytes ratchet;
\t\t\tif (has_ratchet) {
\t\t\t\tratchet = packet.data().mid(KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8, RATCHETSIZE/8);
\t\t\t}
\t\t\tBytes signature = packet.data().mid(KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8 + RATCHET_BYTES, SIGLENGTH/8);
\t\t\tBytes app_data;
\t\t\tif (packet.data().size() > (KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8 + RATCHET_BYTES + SIGLENGTH/8)) {
\t\t\t\tapp_data = packet.data().mid(KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8 + RATCHET_BYTES + SIGLENGTH/8);
\t\t\t}

\t\t\tBytes signed_data;
\t\t\tsigned_data << packet.destination_hash() << public_key << name_hash << random_hash << ratchet << app_data;

\t\t\tif (packet.data().size() <= KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8 + RATCHET_BYTES + SIGLENGTH/8) {
\t\t\t\tapp_data.clear();
\t\t\t}"""


def patch_microreticulum(env):
    # PlatformIO fetches lib_deps into $BUILD_DIR/../libdeps/<env>/<lib>/
    # but the build directory is parameterized per env so we derive it
    # from PROJECT_DIR + the current build env name.
    project_dir = env["PROJECT_DIR"]
    env_name    = env["PIOENV"]
    target = os.path.join(
        project_dir, ".pio", "libdeps", env_name,
        "microReticulum", "src", "Identity.cpp",
    )
    if not os.path.exists(target):
        # Library isn't fetched yet on a fresh clone — this happens on
        # the very first invocation before LDF has pulled lib_deps. The
        # patch will apply on the next run. Print a hint and bail.
        print("pre_build: microReticulum not yet fetched — ratchet patch skipped this pass")
        return

    with open(target, "r", encoding="utf-8", newline="") as f:
        source = f.read()

    # Normalize CRLF -> LF so our Python-literal patch blocks (which
    # always use \n) can match regardless of how the lib was fetched.
    # We'll write back as LF — the C++ compiler doesn't care, and
    # sticking to one style makes the patch idempotent across OSes
    # (CI on Ubuntu vs a dev checkout on Windows).
    source = source.replace("\r\n", "\n")

    if PATCH_MARKER in source:
        print("pre_build: microReticulum ratchet patch already applied (marker present)")
        return

    if ORIGINAL_BLOCK not in source:
        # Upstream changed the block we were targeting. Fail loudly
        # rather than silently shipping unpatched firmware. If this
        # fires, someone needs to re-mine the current upstream source
        # and update ORIGINAL_BLOCK + PATCHED_BLOCK above.
        raise RuntimeError(
            "pre_build: could not locate target block in microReticulum "
            "Identity.cpp for ratchet patch. The upstream source may have "
            "changed since this patch was written — re-mine the current "
            "validate_announce function and update ORIGINAL_BLOCK / "
            "PATCHED_BLOCK in scripts/pre_build.py. See "
            "docs/RATCHET_PROTOCOL.md for context."
        )

    patched = source.replace(ORIGINAL_BLOCK, PATCHED_BLOCK, 1)
    assert PATCH_MARKER in patched, "patch marker not present after replacement — logic bug"

    with open(target, "w", encoding="utf-8", newline="") as f:
        f.write(patched)

    print("pre_build: applied microReticulum ratchet patch to {}".format(target))


patch_microreticulum(env)  # noqa: F821
