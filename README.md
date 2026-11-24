# PowerCanary
Monitoring power systems and emulating a UPS over a serial port.

PowerCanary is firmware for a small ESP32 board that watches a power source and
presents itself to a host computer as a standard serial UPS. The host needs no
custom software: its stock [NUT](https://networkupstools.org/) `nutdrv_qx` driver
(Megatec/Q1 protocol) reads the board and shuts the machine down cleanly on a
real outage.

**Status: early development.** It logs in to a real DELTA 2 Max, and a real
unplug of the power station shows up as `OB` in NUT 2.7.4 and clears again on
replug. On the target host, TrueNAS CORE 13.0, an outage drill ran through
the UPS service to a clean shutdown after ten minutes on battery, and the
service came back by itself on restart. See
[docs/VALIDATION.md](docs/VALIDATION.md).

## Why

A portable power station between the wall and a server hides mains loss from
the server: the server keeps running until the battery is empty, then loses
power without warning. PowerCanary asks the power station directly whether
mains is present and how much charge is left, and reports that to the host in a
form every UPS-aware operating system already understands.

## What it is built for

| Part         | Choice                                                                 |
|--------------|------------------------------------------------------------------------|
| Power source | EcoFlow DELTA 2 Max, read over Bluetooth LE                            |
| Board        | Seeed Studio XIAO ESP32-C5, powered from the host's USB port           |
| Host link    | The board's native USB port as a CDC-ACM serial device, Megatec/Q1     |
| Host         | Anything running NUT; developed against TrueNAS CORE (FreeBSD)         |
| Toolchain    | ESP-IDF 5.5 (CMake)                                                    |

The firmware is **read-only** towards the power station. It reads the AC input
(voltage, current, power) and the battery state of charge, and never writes a
setting.

## Safety semantics

These are the design contract; everything else is secondary.

- **On mains** is reported only from fresh data showing a plausible AC input
  voltage *and* power flowing in. Voltage alone is not evidence: a DELTA 2 Max
  keeps reporting the last voltage it measured after its AC cord is pulled,
  while its input current and power drop to zero.
- **On battery** is reported only after AC input has been absent continuously
  for a debounce period (default 10 s).
- **Low battery** is reported when state of charge is at or below a threshold
  (default 30%). NUT treats on battery + low battery as "shut down now".
- **Stale data is never reported as healthy.** If no data has arrived from the
  power station for a while (default 60 s), the board stops answering the
  status query. NUT then marks the UPS data stale and applies its own
  communication-lost handling. The board never invents either state.
- The shutdown decision rests on those two status bits only. The battery
  voltage in the status reply exists so NUT can display a charge percentage.

Know these limits before relying on it:

- The power station stops sending its inverter heartbeat while its AC output
  is switched off. The board then has no fresh data and goes silent.
- Mains is judged by power flowing in. A power station that is plugged in but
  drawing nothing at all from the wall is reported as on battery.
- The power station accepts one Bluetooth client at a time. While the EcoFlow
  app is connected over Bluetooth, the board cannot connect, and the reverse.
- A firmware update to the power station can change the protocol and break
  this at any time.

## Build and flash

1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32c5/get-started/index.html)
   5.5 or later with the `esp32c5` target, and activate it in your shell.
2. Clone with submodules:

   ```sh
   git clone --recursive https://github.com/AlphaPixel/PowerCanary.git
   cd PowerCanary
   ```
3. Create `main/secrets.h` (see [Secrets](#secrets)).
4. Build and flash, naming the board's serial port:

   ```sh
   idf.py build
   idf.py -p PORT flash
   ```

The firmware stops the USB port's DTR and RTS lines from resetting the chip, so
that a UPS driver opening the port cannot reboot the board. The side effect is
that a board already running PowerCanary will not enter its bootloader on its
own: hold **BOOT**, tap **RESET**, release **BOOT**, then flash.

The XIAO ESP32-C5 has no onboard antenna. Attach the antenna supplied with it
to the U.FL connector, or Bluetooth range will be close to nothing.

Settings live under **PowerCanary** in `idf.py menuconfig`: the debounce time,
the low-battery threshold, the stale-data timeout, the voltage that counts as
mains present, and the nameplate values reported to the host.

## Secrets

The Bluetooth login needs the power station's serial number and the user ID of
the EcoFlow account it is bound to. They live in `main/secrets.h`, which git
ignores. Never commit, log or post them.

The app does not display the user ID. `tools/ecoflow_secrets.py` obtains it the
way the app does, by logging in to EcoFlow's account service:

```sh
python tools/ecoflow_secrets.py
```

It asks for the serial number, your account e-mail and your password. The
password goes to EcoFlow over HTTPS and nowhere else, and the user ID is
written to `main/secrets.h` without being shown. You can also copy
`main/secrets.h.example` and fill it in by hand.

## Using it with NUT

Plug the board into the host. It appears as a USB CDC-ACM serial device:
`/dev/ttyACM0` on Linux, `/dev/cuaU0` on FreeBSD. A minimal `ups.conf` entry:

```ini
[canary]
    driver = nutdrv_qx
    port = /dev/ttyACM0
    protocol = megatec
```

`upsc canary` then shows `ups.status` as `OL` on mains, `OB` on battery and
`OB LB` when the battery is also low.

Unplugging the board, or removing the power station from the power path, makes
NUT report lost communication. Disable the UPS service first if that is
intended.

Settings for TrueNAS CORE are in [docs/truenas-core.md](docs/truenas-core.md).

## Serial protocol

2400 baud is nominal; the USB link ignores the rate. Commands and replies end
with a carriage return.

| Command | Reply                                                                       |
|---------|-----------------------------------------------------------------------------|
| `Q1`    | Megatec status: voltages, load, frequency, battery voltage, eight status bits. No reply at all without fresh data. |
| `F`     | Megatec ratings.                                                            |
| `I`     | Megatec identification.                                                     |
| `DBG`   | One line of diagnostics starting with `!`: Bluetooth state, RSSI, raw readings, data age. Not part of Megatec; NUT never sends it. |
| other   | Echoed back, which is how a Megatec UPS rejects a command. The board has no load control, self-test or beeper. |

`tools/monitor.py PORT` polls `DBG` and `Q1` with timestamps, and has a
`--reopen` mode that checks the board survives the port being opened and
closed.

Logs are written to UART0 only (the D6 pin, 115200 baud), never to the USB
port, so they cannot corrupt a reply. The one exception is the chip's ROM,
which prints a short banner on USB when the chip resets. NUT discards pending
input before each query, so it never sees it.

### Reading `DBG`

| Field | Meaning |
|---|---|
| `ble=S/T` | Link state `S`: 0 waiting to retry, 1 scanning, 2 connecting, 3 handshake, 4 online, 5 closing. `T` is the handshake step reached. |
| `adv` | Advertisements heard from any device. Zero means a radio or antenna problem. |
| `seen` | Advertisements heard from EcoFlow units, any serial. Zero with `adv` above zero means the power station is out of range, switched off, or busy with another Bluetooth client. Above zero while still scanning means the serial number in `secrets.h` is wrong. |
| `rssi` | Signal strength of the power station in dBm. |
| `err=E/D` | Last error: 1 unsupported encryption, 2 GATT, 3 key exchange, 4 login rejected, 5 timeout; `D` is a detail code. |
| `ac_in_mv`, `ac_in_ma`, `in_w` | The AC input as the power station reports it. The voltage keeps its last value when the cord is pulled. |
| `ac` | Whether this sample counts as mains present, before the debounce. |
| `fresh`, `ob`, `lb` | Whether `Q1` would answer, and with which on-battery and battery-low bits. |

### Test builds

`idf.py menuconfig` → PowerCanary → *Accept the SIM and BOOT test commands*
adds three commands. Leave it off in firmware you deploy: anything able to
write to the serial port could then shut the host down.

| Command | Effect |
|---|---|
| `SIM <ac_in_mv> <soc> [in_watts]` | Uses this sample in place of the power station's. `SIM 118455 80 0` reproduces a real outage: the voltage stays, the input power is zero. `SIM OFF` returns to real data. |
| `RAW` | The start of the last two heartbeat payloads in hex, for checking field offsets. |
| `BOOT` | Restarts into the download bootloader without the BOOT button. After flashing, leave it with `python -m esptool --after watchdog_reset chip_id`. |

## Tests

The safety logic, the Megatec responder and the packet codec are plain C with
no platform dependencies, and have a host-side test:

```sh
cmake -S test -B build-host
cmake --build build-host
ctest --test-dir build-host -C Debug
```

## Disclaimer

This is an independent, unofficial project. It is not affiliated with,
endorsed by or supported by EcoFlow, Seeed Studio or the NUT project; product
names are trademarks of their owners. It relies on an undocumented protocol
that can change with any firmware update. A power canary is one layer of
protection, not a guarantee: use it at your own risk and keep a backstop.

## Credits and licenses

PowerCanary is MIT licensed; see [LICENSE](LICENSE). Three parts carry other
terms:

- `main/ef_proto.*`, `main/ecoflow_ble.*` and `main/keydata.bin` are derived
  from [rabits/ha-ef-ble](https://github.com/rabits/ha-ef-ble) and
  [kernelorg/esphome-ecoflow-ble](https://github.com/kernelorg/esphome-ecoflow-ble),
  the protocol references for EcoFlow Bluetooth LE. They are under the
  Apache License 2.0; see [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt).
  The code was rewritten in C and reduced to read-only telemetry.
- `components/micro-ecc/micro-ecc` is
  [kmackay/micro-ecc](https://github.com/kmackay/micro-ecc), BSD 2-Clause, as a
  git submodule.

The Megatec/Q1 formats were checked against the `nutdrv_qx` source of
[Network UPS Tools](https://networkupstools.org/); no NUT code is included.
