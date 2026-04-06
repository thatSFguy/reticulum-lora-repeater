#!/usr/bin/env python3
"""
Send a test announce through the RNode so the Faketec can receive it.
Tests SX1276→SX1262 radio path AND whether our firmware can process
a standard Python Reticulum announce.

Stop rnsd first! This opens COM50 directly.
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

def main():
    RNS.loglevel = RNS.LOG_VERBOSE

    configdir = os.path.join(os.environ.get("USERPROFILE", os.path.expanduser("~")), ".reticulum_diag")
    os.makedirs(configdir, exist_ok=True)

    config_path = os.path.join(configdir, "config")
    with open(config_path, "w") as f:
        f.write(f"""[reticulum]
  enable_transport = False
  share_instance = No

[logging]
  loglevel = 5

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

    print(f"Starting Reticulum with RNode on {PORT}...")
    reticulum = RNS.Reticulum(configdir=configdir)

    # Create an identity and an lxmf.delivery destination — same
    # aspect our repeater announces on
    identity = RNS.Identity()
    dest = RNS.Destination(identity, RNS.Destination.IN, RNS.Destination.SINGLE, "lxmf", "delivery")

    print(f"\nDestination hash: {dest.hexhash}")
    print(f"Identity hash:   {identity.hexhash}")
    print(f"\nSending 5 announces, 3 seconds apart...")
    print(f"Watch the Faketec serial log for Radio: RX pt=ANNOUNCE\n")
    sys.stdout.flush()

    for i in range(5):
        print(f"  Announce {i+1}/5...")
        sys.stdout.flush()
        dest.announce(app_data=b"Python test announce")
        time.sleep(3)

    print("\nDone. Check the Faketec serial log.")
    time.sleep(2)
    reticulum.exit_handler()

if __name__ == "__main__":
    main()
