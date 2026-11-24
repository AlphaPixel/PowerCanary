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

## Results (2026-11-23)

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

## Not yet tested

1. **On the target host.** `umodem` attach and `/dev/cuaU0` on FreeBSD, the
   replies over `cu`, and the reset test repeated there. FreeBSD's serial
   driver may toggle the control lines in a different order from Windows.
2. **RSSI at the installed distance.** The bench figure is from the
   workstation, not from the host's USB port.
3. **A long run.** Nothing longer than about 35 minutes in one session.
4. **A power station drawing nothing from the wall.** Mains is judged by
   power flowing in, so a full unit with no load at all would be reported as
   on battery. Not reproduced: the test unit always carried a load.
5. **The EcoFlow app connecting over Bluetooth** while the board holds the
   link, and the board's recovery afterwards.
