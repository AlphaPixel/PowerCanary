# Using PowerCanary with TrueNAS CORE

These settings were prepared against TrueNAS CORE 13.0, which ships NUT 2.7.4.
Nothing here is applied for you: enabling the UPS service is your decision.

## Before you start

- Plug the board into a USB port on the NAS. It attaches through the `umodem`
  driver and appears as `/dev/cuaU0` (the number rises if other USB serial
  devices are present; `ls /dev/cuaU*` shows them).
- Confirm it answers before involving the UPS service. From a shell on the NAS:

  ```sh
  cu -l /dev/cuaU0 -s 2400
  ```

  Type `Q1` and press Enter. A line starting with `(` and ending in eight
  status digits means the board has fresh data from the power station. No
  reply means it has none yet; `DBG` shows why. Leave `cu` with `~.`.

## Services → UPS

| Setting                         | Value                                                        |
|---------------------------------|--------------------------------------------------------------|
| UPS Mode                        | Master                                                       |
| Identifier                      | `ups` (the default) or a name of your choice                 |
| Driver                          | `Various ups 2 (Various serial) ViewPower software (nutdrv_qx)` |
| Port or Hostname                | `/dev/cuaU0`                                                 |
| Auxiliary Parameters (ups.conf) | `protocol = megatec`                                         |
| Shutdown Mode                   | UPS goes on battery                                          |
| Shutdown Timer                  | `600` (seconds on battery before shutdown)                   |
| Power Off UPS                   | Unset. The board cannot switch the power station off.        |
| No Communication Warning Time   | `300`                                                        |
| Monitor User / Password         | `upsmon` and a password of your own                          |
| Send Email Status Updates       | Set, with your address                                       |

The driver entry only selects `nutdrv_qx`; the label after it is a list entry,
not a model check. `protocol = megatec` skips the driver's protocol
auto-detection. Leave `cablepower` at its default: the default control-line
pattern is the one tested.

### How the two shutdown modes behave

- **UPS goes on battery** with a 600 s timer shuts the NAS down ten minutes
  after mains is lost, however much charge is left. If the charge falls to the
  low-battery threshold first, NUT shuts down at once.
- **UPS reaches low battery** keeps the NAS running on the power station until
  the charge falls to the low-battery threshold (30% unless you rebuilt the
  firmware with another value), then shuts down. This uses more of the
  battery and suits long outages.

Either way the trigger comes from the board's two status bits: on battery,
reported after AC input has been absent for the debounce time, and battery
low.

## What the alerts mean

| NUT event                      | Meaning                                                                 |
|--------------------------------|-------------------------------------------------------------------------|
| On battery (`OB`)              | The power station has lost its AC input for longer than the debounce.   |
| Low battery (`LB`)             | State of charge is at or below the threshold. It can appear on mains, while recharging. |
| Communications lost            | The board has no fresh data: Bluetooth link down, power station's AC output off, or the board unplugged. |

NUT handles lost communication by state:

- On mains, it raises an alert and nothing else.
- While already on battery, it treats the UPS as critical and shuts the NAS
  down. That is deliberate: an outage that also takes out the Bluetooth link
  should still end in a clean shutdown.

The UPS service will not start while the board has no fresh data, because the
driver's first status query goes unanswered. Check `DBG` and start the service
again once `Q1` answers.

## Taking it out of service

If you unplug the board, or take the power station out of the NAS's power
path, stop and disable the UPS service first. Otherwise TrueNAS reports lost
communication until you do.

## Keeping another watchdog

If the NAS already has its own power-loss detection, the two can run side by
side: each shuts the NAS down independently and neither needs the other. A
sensible arrangement is to treat PowerCanary as the primary signal, because it
reads the power station directly, and keep the older mechanism as a backstop
for the case where the board or its Bluetooth link has failed while on mains,
which NUT alone only reports as an alert.
