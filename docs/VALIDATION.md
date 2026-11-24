# Validation log

What has been tested, on what, and what has not. Site details (addresses, host
names, serial numbers) are deliberately left out.

## Test setup

| Item          | Value                                                              |
|---------------|--------------------------------------------------------------------|
| Board         | Seeed Studio XIAO ESP32-C5 with its supplied antenna; chip ESP32-C5HR8, revision v1.0 |
| Power station | EcoFlow DELTA 2 Max, carrying a real load of about 115 W           |
| Toolchain     | ESP-IDF v5.5.6, NimBLE host, micro-ecc at `76fda53`                 |
| Firmware      | 0.1.0, 795 KB, 24% of the default 1 MB app partition free          |
| Workstation   | Windows 11; the board enumerates as USB `303A:1001`, a CDC-ACM serial device using the in-box driver |
| NUT           | 2.7.4 (`nutdrv_qx` 0.28), built from the release tarball in WSL: the version TrueNAS CORE 13.0 ships |
| Target host   | TrueNAS CORE 13.0-U5.3 (FreeBSD 13.1), xHCI USB controller, NUT 2.7.4 as shipped |

## Results (2026-11-23; target host 2026-11-24)

| Check | Result |
|---|---|
| Host-side test of the safety logic, Megatec responder and packet codec | Pass (`ctest`, MSVC `/W4`, no warnings) |
| Session key table | 65,280 bytes; identical in both reference projects |
| Firmware builds for `esp32c5` | Pass, no warnings in project sources |
| Board enumerates as a USB serial device on the workstation | Pass |
| `I`, `F` replies byte for byte, CR terminated | Pass: `#EcoFlow         D2Max      PC-0.1.0  ` (39 bytes), `#120.0 020 048.0 60.0` (22 bytes) |
| `Q1` with no data | Pass: no reply |
| Unimplemented commands (`S03`, `T`) | Pass: echoed back |
| Port open/close does not reset the board | Pass after a fix; see below |
| Bluetooth login to a real DELTA 2 Max | Pass on the first attempt: encryption type 7, key exchange, session key, login |
| Bench run, Bluetooth only | Pass; see below |
| Unplug test on the real power station | Pass after a fix: the first build missed the outage; see below |
| `nutdrv_qx` 2.7.4 with `upsd` and `upsc` | Pass, with simulated and with real outages; see below |
| Simulated outage on the board (test build, `SIM`) | Pass; see below |
| Attach, replies, reset test and RSSI on the target host | Pass, with one host quirk; see below |
| The target host's own `nutdrv_qx`, run by hand | Pass; see below |
| UPS service on the target host, started by TrueNAS | Pass |
| Outage drill through the UPS service, to a full shutdown and restart | Pass; see below |

### Reset on port open and close

A host closing the port with DTR and RTS asserted reset the chip every time
(reset reason: USB peripheral). Each reset would cost a Bluetooth reconnect.

Setting `usb_uart_chip_rst_dis` in the USB serial controller did nothing until
two details were right: the write has to happen after the USB serial driver is
installed, because the controller's register clock is gated off until then,
and it only takes effect once `config_update` is written afterwards.

With both in place, uptime kept counting across separate opens and closes and
across 20 rapid cycles using the control-line pattern `nutdrv_qx` uses
(DTR on, RTS off). `tools/monitor.py PORT --reopen 20` repeats the test.

### Bench run, Bluetooth only

The board on a workstation USB port, polled every 5 s with `tools/monitor.py`.
The distance to the power station was not recorded.

| Measure | Value |
|---|---|
| Duration | 9 min 6 s, 110 polls |
| Sessions / link drops / invalid packets / errors | 1 / 0 / 0 / none |
| `Q1` replies | 110 of 110 well formed, status `00001000` (on line) throughout |
| AC input | 118.0 V to 119.0 V, 60 Hz |
| State of charge | 98% |
| Load | 109 W to 146 W |
| Heartbeat period | Inverter 3.0 s, state of charge 1.0 s |
| Oldest data ever served | 3.0 s |
| RSSI at the bench | Median -70 dBm, range -86 to -58 dBm |

Over the whole evening the link was dropped only by reflashing the board. The
longest single session ran about 35 minutes with no drops or invalid packets,
and after a restart the board was logged in again within 11 s.

### Unplug test

The power station's AC cord was pulled from the wall while it carried its
load, left out, plugged in, pulled again and plugged in again.

**What the power station reports.** Two inverter heartbeats from the same
unit, the second a quarter of an hour into the outage:

| Field (payload offset) | On mains | Cord pulled |
|---|---|---|
| `chargerType` (8) | 0xFF | 0x00 |
| `inputWatts` (9) | 116 | 0 |
| `outputWatts` (11) | 116 | 116 |
| `invOutVol` (14) | 118,109 mV | 119,456 mV |
| `acInVol` (23) | 118,437 mV | **118,455 mV** |
| `acInAmp` (27) | 1,105 mA | 0 |
| `acInFreq` (31) | 60 | 60 |
| `outTemp` (32) | 30 | 39 |

The AC input voltage does not fall when the cord is pulled. It stops updating
and keeps its last value, to the millivolt, for as long as the outage lasts
(checked over 20 minutes). Input current and input power are what change. The
state-of-charge heartbeat also shows it: `chgState` goes from 2 to 1 and
`dsgRemainTime` from its idle value of 5999 to a real figure in minutes.

**The first build got this wrong.** It judged mains by voltage alone, so it
went on reporting "on line" throughout a real outage. The rule is now: mains
is present only when the voltage is plausible and input current or power is
non-zero. An absent input is reported to the host as 0 V rather than as the
stale reading. Both heartbeats above are test vectors in the host-side test.

**With the fix,** flashed while the cord was still out:

| Event | Board | `Q1` reply |
|---|---|---|
| Restart during the outage | On battery 10 s after its first sample | `(000.0 000.0 119.5 005 00.0 51.6 00.0 10001000` |
| Cord plugged in | On line | `(118.6 118.6 117.9 004 60.0 51.5 00.0 00001000` |
| Cord pulled again | On battery | `(000.0 000.0 119.2 004 00.0 51.7 00.0 10001000` |
| Cord plugged in again | On line, after 86 s reported on battery | `(116.9 116.9 116.5 004 60.0 51.7 00.0 00001000` |

While the unit recharged afterwards its input voltage read as low as 104 V,
still well above the 50 V floor.

### NUT 2.7.4

`nutdrv_qx`, `upsd` and `upsc` from NUT 2.7.4, run unprivileged in WSL with
`protocol = megatec`. The serial port was bridged from the workstation to a
pseudo-terminal, so this exercises the protocol and the driver's parsing but
not the control lines; those are covered by the reopen test above.

The driver claimed the device on its first exchange (`Q1`, `I`, `F`, `I`).
What `upsc` showed on mains:

```
battery.charge: 100
battery.voltage: 52.00
battery.voltage.high: 52.00
battery.voltage.low: 41.60
battery.voltage.nominal: 48.0
device.mfr: EcoFlow
device.model: D2Max
input.frequency: 60.0
input.voltage: 116.9
output.voltage: 116.5
ups.firmware: PC-0.1.0
ups.load: 5
ups.status: OL
```

`battery.charge` is NUT's own estimate from the battery voltage. It stayed
within one percent of the power station's state of charge.

`ups.status` as `upsc` reported it, first with samples injected by `SIM`:

| Injected sample | `ups.status` |
|---|---|
| Mains, 80% | `OL` |
| Input power stops, 80% | `OL` through the debounce, then `OB` |
| Input power stopped, 25% | `OB LB` |
| Input power back, 25% | `OL LB` |
| Real data again | `OL` |

The driver polls the status bits every 2 s but refreshes the other values
only every 30 s (`pollfreq`), so `battery.charge` and the voltages can lag a
status change by up to that long.

Returning to real data clears the board's state, so one status query went
unanswered until the next heartbeat arrived. The driver logged a failed read
and carried on at the next poll.

Then the real power station, with the driver watching:

| Event | `ups.status` | Notes |
|---|---|---|
| Driver started during the real outage | `OB` | `input.voltage: 0.0`, `output.voltage: 119.5` |
| Cord plugged in | `OL` | About a second after the board |
| Cord pulled again | `OB` | About a second after the board |
| Cord plugged in again | `OL` | About a second after the board |

### Simulated outage on the board

Samples injected with the test-build `SIM` command, replies read from the real
board over USB. The voltage reading is left in place and only the input power
is removed, as in a real outage.

| Step | `Q1` reply | Meaning |
|---|---|---|
| Mains present, 80% | `(118.5 118.5 120.0 000 60.0 49.9 00.0 00001000` | On line |
| Input power stops | `(000.0 000.0 120.0 000 00.0 49.9 00.0 00001000` | Still on line: inside the 10 s debounce |
| 11 s later | `(000.0 000.0 120.0 000 00.0 49.9 00.0 10001000` | On battery |
| Charge at 25% | `(000.0 000.0 120.0 000 00.0 44.2 00.0 11001000` | On battery, battery low |
| Input power back at 25% | `(118.5 118.5 120.0 000 60.0 44.2 00.0 01001000` | On line at once; battery still low |
| 54 s after the last sample | reply as before | Data still fresh |
| 62 s after the last sample | no reply | Stale: status query unanswered, `F` still answers |

### On the target host (2026-11-24)

The board on a USB port of the NAS it protects, release firmware, checked
over SSH. The UPS service stayed disabled and stopped throughout; where the
driver was needed it was run by hand with a throwaway configuration.

| Check | Result |
|---|---|
| Attach | `umodem` loaded by itself; `/dev/cuaU0` is owned by `uucp:dialer`, and NUT runs as `uucp` |
| `I`, `F`, `Q1`, unknown command | Same replies as on the workstation |
| `cu -l /dev/cuaU0 -s 2400` | Works, subject to the quirk below |
| Reset test | No reset: uptime kept counting across 20 plain and 20 driver-style reopens, and across more than 100 other reopens during these tests |
| RSSI at the installed position | -55 to -48 dBm (the bench median was -70 dBm) |
| Bluetooth session | One session, no drops, no invalid packets in the first 27 minutes |
| `nutdrv_qx` 0.28 as shipped | Claims Megatec, `ups.status: OL`, the same variables as the WSL run |
| Driver stop (SIGTERM) | Exits within 0.1 s and removes its lock and PID file |

**Host quirk: the first reply after a reopen can be lost.** When the port has
been closed and opened again, this host sometimes discards the first USB
packet the board sends. A short reply (`Q1`, `I`, `F`) fits in one packet and
is lost whole; a `DBG` reply loses its first 64 bytes. Everything after that
packet arrives. Waiting does not help: a query sent 2 s after the open was
lost as readily as one sent at once.

Whether the packet is lost depends only on whether the board has sent an odd
or an even number of packets since the previous open, zero-length packets
included. One `DBG` per open (an odd number) made the losses alternate, open
by open; one `F` per open (an even number) never changed the state. That is
the signature of a USB data toggle left out of step: the host restarts its
side of the pipe on reopen and the board does not. The traffic was not
captured at USB level, so the mechanism is inferred from the pattern. The
Windows workstation did not show it.

`nutdrv_qx` rides through it. Started with the port deliberately left in the
losing state:

```
0.106582  send: 'Q1'
1.135766  read: timeout (0)
1.135808  send: 'Q1'
1.136332  read: '(116.9 116.9 116.6 004 60.0 51.9 00.0 00001000'
1.136351  send: 'I'
1.136570  read: '#EcoFlow         D2Max      PC-0.1.0  '
1.136578  Using protocol: Megatec 0.06
```

The cost is one second at start-up. It started on five attempts out of five:
two with the port in the losing state, three taking over from a running
instance. A running driver keeps the port open, so the loss cannot recur
until the next start.

**The UPS service.** Configured as in [truenas-core.md](truenas-core.md) and
started through TrueNAS: the driver, `upsd` and `upsmon` came up, `upsc`
reported `ups.status: OL` with the same variables as above, and the generated
`upssched.conf` carries `AT ONBATT * START-TIMER SHUTDOWN 600`. TrueNAS
raised no alert. It then ran for 73 minutes on the same driver process
before the drill.

**Outage drill.** The power station's cord was pulled with the service
running and left out until the host shut itself down.

| Time | Event |
|---|---|
| 10:27:30 | `ups.status` goes from `OL` to `OB`; `upsmon` logs "on battery"; the shutdown timer starts; the on-battery e-mail arrives |
| 10:37:32 | Timer expires after 10 min 2 s: "issuing shutdown", then `upsmon` "Executing automatic power-fail shutdown" |
| 10:37:37 | `shutdown`: "power-down by root" |
| 10:37:44 | Host no longer answers on the network |
| 10:42:10 | Cord back in; host powered on by hand and booting |
| 10:44 | Host up; the board is back as `/dev/cuaU0`; the UPS service has started by itself and reports `OL`; pools online with no errors |

The service starting by itself means the board had fresh data from the
power station by the time the driver first asked, about two minutes into
the boot.

## Not yet tested

1. **Return of mains cancelling the shutdown timer** on the target host. The
   drill let the timer run out instead.
2. **A long run.** Nothing longer than 73 minutes in one session.
3. **A power station drawing nothing from the wall.** Mains is judged by
   power flowing in, so a full unit with no load at all would be reported as
   on battery. Not reproduced: the test unit always carried a load.
4. **The EcoFlow app connecting over Bluetooth** while the board holds the
   link, and the board's recovery afterwards.
