#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write main/secrets.h: the power station's serial number and your EcoFlow user ID.

The user ID is not shown in the EcoFlow app. This script gets it the way the
app does, by logging in to EcoFlow's account service with your e-mail address
and password. Your password is sent to EcoFlow over HTTPS and nowhere else; it
is not stored. The user ID is written to main/secrets.h and never printed.

Run it yourself, in your own terminal:

    python tools/ecoflow_secrets.py

Standard library only. The login request follows rabits/ha-ef-ble (Apache-2.0).
"""
import argparse
import base64
import getpass
import json
import pathlib
import re
import sys
import urllib.error
import urllib.request

OUT = pathlib.Path(__file__).resolve().parent.parent / "main" / "secrets.h"


def fetch_user_id(host: str, email: str, password: str) -> str:
    body = {
        "scene": "IOT_APP",
        "appVersion": "1.0.0",
        "password": base64.b64encode(password.encode()).decode(),
        "oauth": {"bundleId": "com.ef.EcoFlow"},
        "userType": "ECOFLOW",
        "email": email,
    }
    request = urllib.request.Request(
        f"https://{host}/auth/login",
        data=json.dumps(body).encode(),
        headers={"Accept": "application/json", "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            reply = json.load(response)
    except urllib.error.URLError as e:
        sys.exit(f"Could not reach {host}: {e}")
    if str(reply.get("code")) != "0":
        sys.exit(f"EcoFlow refused the login: {reply.get('message', 'no reason given')}")
    return str(reply["data"]["user"]["userId"])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--host",
        default="api.ecoflow.com",
        help="EcoFlow account server; e.g. api-e.ecoflow.com for Europe (default: %(default)s)",
    )
    args = parser.parse_args()

    serial = input("Power station serial number (16 characters, on the label): ").strip().upper()
    if not re.fullmatch(r"[0-9A-Z]{16}", serial):
        sys.exit("A serial number is 16 letters and digits.")
    if not serial.startswith(("R351", "R354")):
        print("Note: that is not a DELTA 2 Max serial (R351... or R354...); other models are untested.")
    email = input("EcoFlow account e-mail: ").strip()
    password = getpass.getpass("EcoFlow account password (not shown, not stored): ")

    user_id = fetch_user_id(args.host, email, password)
    if not re.fullmatch(r"[0-9A-Za-z]{1,40}", user_id):
        sys.exit("EcoFlow returned a user ID in an unexpected form; nothing written.")

    OUT.write_text(
        "/* Written by tools/ecoflow_secrets.py. Never commit, log or post this file. */\n"
        "#pragma once\n"
        f'#define ECOFLOW_SERIAL "{serial}"\n'
        f'#define ECOFLOW_USER_ID "{user_id}"\n',
        encoding="ascii",
    )
    print(f"Wrote {OUT}. The user ID was not displayed; rebuild and flash to use it.")


if __name__ == "__main__":
    try:
        main()
    except EOFError:
        print()
        sys.exit("No keyboard input available: run this in an interactive terminal window.")
    except KeyboardInterrupt:
        print()
        sys.exit("Cancelled; nothing written.")
