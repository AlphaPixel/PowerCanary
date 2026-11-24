# Validation log

What has been tested, on what, and what has not. Site details (addresses, host
names, serial numbers) are deliberately left out.

## Test setup

| Item        | Value                                                              |
|-------------|--------------------------------------------------------------------|
| Board       | Seeed Studio XIAO ESP32-C5; chip ESP32-C5HR8, revision v1.0        |
| Toolchain   | ESP-IDF v5.5.6, NimBLE host, micro-ecc at `76fda53`                 |
| Firmware    | 0.1.0, 793 KB, 24% of the default 1 MB app partition free          |
| Workstation | Windows 11; the board enumerates as USB `303A:1001`, a CDC-ACM serial device using the in-box driver |
| Target host | TrueNAS CORE 13.0 with NUT 2.7.4 (`nutdrv_qx` 0.28)                 |

## Results so far (2026-11-23)

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
| Simulated outage on the board (test build, `SIM`) | Pass; see below |
| Bluetooth login to a real DELTA 2 Max | Pass on the first attempt: encryption type 7, key exchange, session key, login |
| Field offsets against the real unit | Pass: raw payloads decode to plausible values at the offsets taken from the reference |
| Bench run, Bluetooth only | Pass; see below |

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

### Simulated outage

Samples were injected with the test-build `SIM` command; the replies came from
the real board over USB.

| Step | `Q1` reply | Meaning |
|---|---|---|
| Mains present, 80% | `(120.0 120.0 120.0 000 60.0 49.9 00.0 00001000` | On line |
| Mains lost, 0 s and 5 s later | `(000.0 000.0 120.0 000 00.0 49.9 00.0 00001000` | Still on line: inside the 10 s debounce |
| 12 s after loss | `(000.0 000.0 120.0 000 00.0 49.9 00.0 10001000` | On battery |
| Charge at 25% | `(000.0 000.0 120.0 000 00.0 44.2 00.0 11001000` | On battery, battery low |
| Mains back at 25% | `(120.0 120.0 120.0 000 60.0 44.2 00.0 01001000` | On line at once; battery still low |
| 55 s after the last sample | same | Data still fresh |
| 63 s after the last sample | no reply | Stale: status query unanswered, `F` still answers |

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

A raw inverter payload from this unit, decoded at the reference offsets:
output 116 W at offset 11, inverter output 118,109 mV at 14, AC input
118,437 mV at 23, AC input frequency 60 Hz at 31. State of charge 98 at offset
14 of the other heartbeat.

## Not yet tested

1. **RSSI at the installed distance.** The bench figure above is from the
   workstation, not from the host's USB port.
2. **Unplug test.** AC input pulled from the power station: the field values
   before and after, and the on-battery transition and recovery.
3. **`nutdrv_qx` on a workstation.** `upsc` showing `OL`, `OB`, `OB LB`.
4. **On the NAS.** `umodem` attach, `/dev/cuaU0`, replies over `cu`, and the
   reset test repeated under FreeBSD.
