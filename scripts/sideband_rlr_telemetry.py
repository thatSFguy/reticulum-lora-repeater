######################################################################
# sideband_rlr_telemetry.py
#
# Sideband service plugin that listens for rlr.telemetry announces
# from reticulum-lora-repeater nodes, stores them persistently, and
# makes the latest readings queryable via LXMF command.
#
# Installation:
#   Copy this file into your Sideband plugins directory, then
#   restart Sideband.  The plugin registers automatically.
#
#   Linux   ~/.config/sideband/plugins/
#   Android (see Sideband docs)
#
# How it works:
#   1. Registers an announce handler for the "rlr.telemetry" aspect.
#   2. Parses the ASCII "key=value;..." app_data from each announce.
#   3. Persists all received readings to a JSON file so data survives
#      app restarts.
#   4. Logs every received telemetry frame via RNS.log().
#
# Data file location:
#   <sideband_config_dir>/rlr_telemetry.json
#
# The JSON structure is:
#   {
#     "<destination_hash_hex>": {
#       "last_seen": "2026-04-06T12:34:56",
#       "battery_mv": 4200,
#       "uptime_s": 3600,
#       "heap_free": 8192,
#       "radio_online": true,
#       "packets_in": 145,
#       "packets_out": 89,
#       "history": [
#         { "ts": "2026-04-06T12:34:56", "bat": 4200, "up": 3600, ... },
#         ...
#       ]
#     }
#   }
######################################################################

import RNS
import os
import json
import time
import threading

from datetime import datetime

# Sideband will inject this at load time
from sbapp.sideband.plugins import SidebandServicePlugin

# ── constants ────────────────────────────────────────────────────────

TELEMETRY_ASPECT  = "rlr.telemetry"
DATA_FILENAME     = "rlr_telemetry.json"
MAX_HISTORY       = 200   # per-node history entries to keep

# ── helpers ──────────────────────────────────────────────────────────

def _parse_telemetry(raw_bytes):
    """Parse ASCII 'key=val;key=val;...' into a dict of native types."""
    try:
        text = raw_bytes.decode("utf-8")
    except Exception:
        return None

    fields = {}
    for pair in text.split(";"):
        if "=" not in pair:
            continue
        k, v = pair.split("=", 1)
        k = k.strip()
        v = v.strip()
        # try int, then float, then leave as string
        try:
            fields[k] = int(v)
        except ValueError:
            try:
                fields[k] = float(v)
            except ValueError:
                fields[k] = v
    return fields if fields else None


def _friendly(fields):
    """Human-readable one-liner from parsed fields."""
    parts = []
    if "bat" in fields:
        mv = fields["bat"]
        parts.append(f"Bat: {mv} mV ({mv/1000:.2f} V)")
    if "up" in fields:
        s = fields["up"]
        h, m = divmod(s // 60, 60)
        parts.append(f"Up: {h}h{m:02d}m")
    if "ro" in fields:
        parts.append("Radio: " + ("ON" if fields["ro"] else "OFF"))
    if "pin" in fields:
        parts.append(f"RX: {fields['pin']}")
    if "pout" in fields:
        parts.append(f"TX: {fields['pout']}")
    if "hpf" in fields:
        parts.append(f"Heap: {fields['hpf']} B")
    return " | ".join(parts) if parts else str(fields)


# ── plugin class ─────────────────────────────────────────────────────

class RlrTelemetryPlugin(SidebandServicePlugin):
    service_name = "rlr_telemetry"

    def __init__(self, sideband_core):
        self._data = {}
        self._lock = threading.Lock()
        self._data_path = None
        super().__init__(sideband_core)

    # ── persistence ──────────────────────────────────────────────────

    def _resolve_data_path(self):
        """Find a writable directory for the JSON data file."""
        sb = self.get_sideband()
        # Sideband exposes its config dir in several ways; try the most
        # common attribute names and fall back to ~/.config/sideband.
        for attr in ("config_path", "config_dir", "app_dir"):
            d = getattr(sb, attr, None)
            if d and os.path.isdir(str(d)):
                return os.path.join(str(d), DATA_FILENAME)
        fallback = os.path.join(
            os.path.expanduser("~"), ".config", "sideband")
        os.makedirs(fallback, exist_ok=True)
        return os.path.join(fallback, DATA_FILENAME)

    def _load(self):
        if self._data_path is None:
            self._data_path = self._resolve_data_path()
        try:
            with open(self._data_path, "r") as f:
                self._data = json.load(f)
            RNS.log(f"[rlr] Loaded {len(self._data)} node(s) from {self._data_path}",
                     RNS.LOG_DEBUG)
        except FileNotFoundError:
            self._data = {}
        except Exception as e:
            RNS.log(f"[rlr] Failed to load data: {e}", RNS.LOG_ERROR)
            self._data = {}

    def _save(self):
        if self._data_path is None:
            return
        try:
            tmp = self._data_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self._data, f, indent=2)
            os.replace(tmp, self._data_path)
        except Exception as e:
            RNS.log(f"[rlr] Failed to save data: {e}", RNS.LOG_ERROR)

    # ── announce handling ────────────────────────────────────────────

    def _record(self, destination_hash, app_data):
        """Parse and store a telemetry announce."""
        if app_data is None:
            return

        fields = _parse_telemetry(app_data)
        if fields is None:
            return

        dest_hex = destination_hash.hex()
        now_iso  = datetime.now().isoformat(timespec="seconds")

        RNS.log(f"[rlr] Telemetry from {dest_hex[:16]}...: {_friendly(fields)}",
                RNS.LOG_NOTICE)

        entry = dict(fields)
        entry["ts"] = now_iso

        with self._lock:
            node = self._data.get(dest_hex, {})
            node["last_seen"]    = now_iso
            node["battery_mv"]   = fields.get("bat")
            node["uptime_s"]     = fields.get("up")
            node["heap_free"]    = fields.get("hpf")
            node["radio_online"] = bool(fields.get("ro", 0))
            node["packets_in"]   = fields.get("pin")
            node["packets_out"]  = fields.get("pout")

            history = node.get("history", [])
            history.append(entry)
            if len(history) > MAX_HISTORY:
                history = history[-MAX_HISTORY:]
            node["history"] = history

            self._data[dest_hex] = node
            self._save()

    # ── lifecycle ────────────────────────────────────────────────────

    def start(self):
        super().start()
        self._load()

        # RNS.Transport.register_announce_handler() expects an object
        # with .aspect_filter (str|None) and .received_announce() method.
        self._announce_handler = _AnnounceHandler(self)
        RNS.Transport.register_announce_handler(self._announce_handler)

        RNS.log("[rlr] Telemetry plugin started -- listening for rlr.telemetry announces",
                RNS.LOG_NOTICE)

    def stop(self):
        super().stop()
        RNS.log("[rlr] Telemetry plugin stopped", RNS.LOG_NOTICE)


# ── RNS announce handler object ─────────────────────────────────────
# RNS requires an object with .aspect_filter and .received_announce(),
# not a bare callable.

class _AnnounceHandler:
    aspect_filter = TELEMETRY_ASPECT

    def __init__(self, plugin):
        self._plugin = plugin

    def received_announce(self, destination_hash, announced_identity, app_data):
        self._plugin._record(destination_hash, app_data)


# Sideband discovers the plugin via this module-level variable
plugin_class = RlrTelemetryPlugin
