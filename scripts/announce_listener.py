#!/usr/bin/env python3
"""
Standalone announce listener — bypasses rnsd, opens the RNode directly.
Stop rnsd first so the COM port is free!

Usage:
    1. Stop rnsd (Ctrl+C in its terminal)
    2. python scripts/announce_listener.py
    3. Send ANNOUNCE on the repeater
"""

import RNS
import time
import sys
import os

PORT = "COM50"
FREQ = 904375000
BW   = 250000
SF   = 10
CR   = 5
TXP  = 2

def announce_handler(destination_hash, announced_identity, app_data):
    ts = time.strftime("%H:%M:%S")
    print(f"\n{'='*60}")
    print(f"  [{ts}] *** ANNOUNCE ACCEPTED ***")
    print(f"  destination: {destination_hash.hex()}")
    if announced_identity:
        print(f"  identity:    {RNS.Identity.truncated_hash(announced_identity.get_public_key()).hex()}")
    if app_data:
        print(f"  app_data:    {app_data.hex()}")
        try:
            print(f"  app_text:    {app_data}")
        except:
            pass
    print(f"{'='*60}\n")
    sys.stdout.flush()

def main():
    # Force maximum logging to stdout
    RNS.loglevel = RNS.LOG_EXTREME
    RNS.logtimefmt = "%H:%M:%S"

    # Wipe default config so we don't conflict with rnsd
    configdir = os.path.join(os.environ.get("USERPROFILE", os.path.expanduser("~")), ".reticulum_diag")
    os.makedirs(configdir, exist_ok=True)

    # Write a minimal config with just the RNode
    config_path = os.path.join(configdir, "config")
    with open(config_path, "w") as f:
        f.write(f"""[reticulum]
  enable_transport = False
  share_instance = No

[logging]
  loglevel = 7

[interfaces]
  [[RNode USB]]
    type = RNodeInterface
    interface_enabled = True
    port = {PORT}
    frequency = {FREQ}
    bandwidth = {BW}
    spreadingfactor = {SF}
    codingrate = {CR}
    txpower = {TXP}
""")

    print(f"Starting standalone Reticulum with RNode on {PORT}...")
    print(f"Config at {configdir}")
    print(f"Radio: {FREQ/1e6:.3f} MHz, BW={BW/1000}kHz, SF={SF}, CR=4/{CR}")
    print()
    sys.stdout.flush()

    reticulum = RNS.Reticulum(configdir=configdir)

    RNS.Transport.register_announce_handler(announce_handler)

    print(f"\nListening... send ANNOUNCE on the repeater.")
    print(f"Press Ctrl+C to quit.\n")
    sys.stdout.flush()

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nDone.")

if __name__ == "__main__":
    main()
