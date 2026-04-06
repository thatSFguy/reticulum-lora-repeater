######################################################################
# sideband_rlr_telemetry_view.py
#
# Sideband telemetry plugin that displays the latest readings from
# reticulum-lora-repeater nodes as Custom sensor entries on the local
# device's telemetry screen.
#
# This plugin reads from the same rlr_telemetry.json file that the
# companion service plugin (sideband_rlr_telemetry.py) writes.  You
# can run both side-by-side:
#
#   sideband_rlr_telemetry.py      — captures announces, persists data
#   sideband_rlr_telemetry_view.py — displays latest values in-app
#
# Installation:
#   Copy both files into your Sideband plugins directory, then
#   restart Sideband.
#
#   Linux   ~/.config/sideband/plugins/
#   Android (see Sideband docs)
#
# The custom sensor entries appear under your local device's telemetry
# view (Object Details).  Each known repeater gets a group of entries
# showing battery, uptime, radio status, packet counts, and heap.
######################################################################

import RNS
import os
import json

# NOTE: Do NOT import SidebandTelemetryPlugin — Sideband's plugin loader
# uses exec() and injects the base classes into the namespace automatically.

DATA_FILENAME = "rlr_telemetry.json"


def _resolve_data_path(sideband):
    """Find the JSON file written by the service plugin."""
    for attr in ("config_path", "config_dir", "app_dir"):
        d = getattr(sideband, attr, None)
        if d and os.path.isdir(str(d)):
            return os.path.join(str(d), DATA_FILENAME)
    return os.path.join(
        os.path.expanduser("~"), ".config", "sideband", DATA_FILENAME)


def _format_uptime(seconds):
    if seconds is None:
        return "?"
    d, rem = divmod(int(seconds), 86400)
    h, rem = divmod(rem, 3600)
    m, _   = divmod(rem, 60)
    if d > 0:
        return f"{d}d {h}h {m:02d}m"
    return f"{h}h {m:02d}m"


def _format_heap(bytes_free):
    if bytes_free is None:
        return "?"
    if bytes_free >= 1024:
        return f"{bytes_free / 1024:.1f} KB"
    return f"{bytes_free} B"


class RlrTelemetryViewPlugin(SidebandTelemetryPlugin):
    plugin_name = "rlr_telemetry_view"

    def start(self):
        super().start()
        RNS.log("[rlr-view] Telemetry view plugin started", RNS.LOG_NOTICE)

    def stop(self):
        super().stop()
        RNS.log("[rlr-view] Telemetry view plugin stopped", RNS.LOG_NOTICE)

    def update_telemetry(self, telemeter):
        data_path = _resolve_data_path(self.get_sideband())

        try:
            with open(data_path, "r") as f:
                data = json.load(f)
        except FileNotFoundError:
            return
        except Exception as e:
            RNS.log(f"[rlr-view] Failed to read {data_path}: {e}",
                    RNS.LOG_WARNING)
            return

        if not data:
            return

        telemeter.synthesize("custom")
        sensor = telemeter.sensors["custom"]

        for dest_hex, node in data.items():
            short_id = dest_hex[:8]
            name = node.get("display_name", short_id)
            label_prefix = f"RLR {name}"
            last_seen = node.get("last_seen", "?")

            bat_mv = node.get("battery_mv")
            bat_str = f"{bat_mv} mV ({bat_mv / 1000:.2f} V)" if bat_mv else "?"

            radio = "Online" if node.get("radio_online") else "Offline"
            pkts_in  = node.get("packets_in", "?")
            pkts_out = node.get("packets_out", "?")

            sensor.update_entry(
                last_seen,
                type_label=f"{label_prefix} Last Seen",
                custom_icon="clock-outline",
            )
            sensor.update_entry(
                bat_str,
                type_label=f"{label_prefix} Battery",
                custom_icon="battery",
            )
            sensor.update_entry(
                _format_uptime(node.get("uptime_s")),
                type_label=f"{label_prefix} Uptime",
                custom_icon="timer-outline",
            )
            sensor.update_entry(
                radio,
                type_label=f"{label_prefix} Radio",
                custom_icon="access-point",
            )
            sensor.update_entry(
                f"RX {pkts_in} / TX {pkts_out}",
                type_label=f"{label_prefix} Packets",
                custom_icon="swap-vertical",
            )
            sensor.update_entry(
                _format_heap(node.get("heap_free")),
                type_label=f"{label_prefix} Heap Free",
                custom_icon="memory",
            )


plugin_class = RlrTelemetryViewPlugin
