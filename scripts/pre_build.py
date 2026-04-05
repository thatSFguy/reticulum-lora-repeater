#
# reticulum-lora-repeater / scripts/pre_build.py
# -------------------------------------------------------------------
# PlatformIO pre-build script. The only thing this needs to do today
# is strip --specs=nano.specs from LINKFLAGS on nordicnrf52 builds so
# C++ exceptions actually work at runtime.
#
# Context (don't delete this without reading it):
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
#   std::runtime_error from several legitimate error paths
#   (FileSystem not registered, Identity load failures, etc.), so
#   without this fix the firmware boots in complete silence on any
#   unexpected runtime error.
#
#   The sibling project microReticulum_Faketec_Repeater has this same
#   fix in its own extra_script.py; see its lines 219-222. We extract
#   just that one operation into this file so we don't drag in the
#   rnodeconf provisioning / firmware_hash / packaging logic we don't
#   use.
#
# Loaded via `extra_scripts = pre:scripts/pre_build.py` in platformio.ini
# under [env:Faketec] (and any other nRF52 env that needs exceptions).

Import("env")

platform = env.GetProjectOption("platform")
if platform == "nordicnrf52":
    if "--specs=nano.specs" in env["LINKFLAGS"]:
        env["LINKFLAGS"].remove("--specs=nano.specs")
        print("pre_build: removed --specs=nano.specs from LINKFLAGS (C++ exceptions fix)")
    else:
        # If this ever starts printing, the platform changed its defaults
        # and we should verify exceptions still work.
        print("pre_build: --specs=nano.specs not found in LINKFLAGS (may be already removed)")
