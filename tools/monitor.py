#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Talk to a PowerCanary board the way a UPS driver does, and log what it says.

    python tools/monitor.py COM11                  poll DBG and Q1 every 5 s
    python tools/monitor.py /dev/ttyACM0 -i 2      poll every 2 s
    python tools/monitor.py COM11 -n 120           poll 120 times, then stop
    python tools/monitor.py COM11 --once I F Q1    send these commands once
    python tools/monitor.py COM11 --reopen 20      reset test: open and close the port 20 times

Each line is timestamped, so the output is a validation log. A command with no
reply within a second prints <no reply>, which is the correct answer to Q1
while the board has no fresh data. Needs pyserial (ESP-IDF's Python has it).
"""
import argparse
import datetime
import time

import serial


def ask(port: serial.Serial, command: str) -> str:
    port.reset_input_buffer()
    port.write(command.encode("ascii") + b"\r")
    reply = port.read_until(b"\r")
    return reply.decode("ascii", "replace").rstrip("\r") if reply.endswith(b"\r") else "<no reply>"


def stamp() -> str:
    return datetime.datetime.now().isoformat(timespec="seconds")


def uptime(port: serial.Serial) -> int:
    fields = dict(f.split("=", 1) for f in ask(port, "DBG").lstrip("!").split() if "=" in f)
    return int(fields.get("up", -1))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port")
    parser.add_argument("-i", "--interval", type=float, default=5.0, help="seconds between polls (default 5)")
    parser.add_argument("-n", "--count", type=int, default=0, help="stop after this many polls (default: run until Ctrl-C)")
    parser.add_argument("--once", nargs="+", metavar="CMD", help="send these commands once and exit")
    parser.add_argument("--reopen", type=int, metavar="N", help="open and close the port N times, then report")
    args = parser.parse_args()

    # 2400 baud and DTR/RTS asserted on open, as nutdrv_qx does it.
    def open_port() -> serial.Serial:
        return serial.Serial(args.port, 2400, timeout=1.0)

    if args.reopen:
        with open_port() as port:
            before = uptime(port)
        for _ in range(args.reopen):
            with open_port() as port:
                port.dtr, port.rts = True, False  # nutdrv_qx "cablepower = normal"
                time.sleep(0.2)
        time.sleep(1.0)
        with open_port() as port:
            after = uptime(port)
        verdict = "no reset" if after >= before >= 0 else "BOARD RESET OR NOT ANSWERING"
        print(f"{stamp()} uptime before {before} s, after {args.reopen} reopens {after} s: {verdict}")
        return

    with open_port() as port:
        if args.once:
            for command in args.once:
                print(f"{stamp()} {command:<4} -> {ask(port, command)}")
            return
        polls = 0
        while not args.count or polls < args.count:
            for command in ("DBG", "Q1"):
                print(f"{stamp()} {command:<4} -> {ask(port, command)}", flush=True)
            polls += 1
            time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
