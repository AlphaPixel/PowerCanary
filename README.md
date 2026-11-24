# PowerCanary
Monitoring power systems and emulating a UPS over a serial port.

PowerCanary is firmware for a small ESP32 board that watches a power source and
presents itself to a host computer as a standard serial UPS. The host needs no
custom software: its stock [NUT](https://networkupstools.org/) `nutdrv_qx` driver
(Megatec/Q1 protocol) reads the board and shuts the machine down cleanly on a
real outage.

**Status: early development. There is nothing to flash yet.**

## Why

A portable power station between the wall and a server hides mains loss from
the server: the server keeps running until the battery is empty, then loses
power without warning. PowerCanary asks the power station directly whether
mains is present and how much charge is left, and reports that to the host in a
form every UPS-aware operating system already understands.

## First target

| Part         | Choice                                                                 |
|--------------|------------------------------------------------------------------------|
| Power source | EcoFlow DELTA 2 Max, read over Bluetooth LE                            |
| Board        | Seeed Studio XIAO ESP32-C5, powered from the host's USB port           |
| Host link    | The board's native USB port as a CDC-ACM serial device, Megatec/Q1     |
| Host         | Anything running NUT; developed against TrueNAS CORE (FreeBSD)         |
| Toolchain    | ESP-IDF (CMake)                                                        |

The firmware is **read-only** towards the power station. It reads AC input
presence and battery state of charge, and never writes a setting.

## Safety semantics

These are the design contract; everything else is secondary.

- **On mains** is reported only from fresh data showing AC input present.
- **On battery** is reported only after AC input has been absent continuously
  for a debounce period.
- **Low battery** is reported when state of charge falls to a configurable
  threshold. NUT treats on battery + low battery as "shut down now".
- **Stale data is never reported as healthy.** If the power station stops
  answering, the board stops answering the host, and NUT raises its own
  communication-lost handling. The board never invents either state.

## Secrets

The power station's serial number and the owner's account user ID are needed
for the Bluetooth login. They live in an untracked `secrets.h` and must never
be committed, logged or posted in an issue.

## Disclaimer

This is an independent, unofficial project. It is not affiliated with,
endorsed by or supported by EcoFlow, Seeed Studio or the NUT project; product
names are trademarks of their owners. It relies on an undocumented protocol
that can change with any firmware update. A power canary is one layer of
protection, not a guarantee: use it at your own risk and keep a backstop.

## Credits

- [rabits/ha-ef-ble](https://github.com/rabits/ha-ef-ble) (Apache-2.0), the
  protocol reference for EcoFlow Bluetooth LE.
- [Network UPS Tools](https://networkupstools.org/), whose `nutdrv_qx` source
  is the authority for the Megatec/Q1 formats.

## License

MIT. See [LICENSE](LICENSE).
