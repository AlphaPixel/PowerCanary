/* SPDX-License-Identifier: MIT */
#include "megatec.h"

#include <stdio.h>
#include <string.h>

/* The battery is reported as a 48 V pack. nutdrv_qx has no state-of-charge
 * field in this protocol: it derives battery.charge from the Q1 battery
 * voltage, between low = nominal * 104/120 and high = nominal * 130/120.
 * With a 48.0 V nominal that is 41.6 V to 52.0 V, so one percent of charge is
 * about 0.1 V, the resolution of the field. The value is informational only:
 * shutdown decisions come from the status bits. */
#define BATT_NOMINAL_DV 480u
#define BATT_LOW_DV (BATT_NOMINAL_DV * 104u / 120u)
#define BATT_HIGH_DV (BATT_NOMINAL_DV * 130u / 120u)

/* #<company 15> <model 10> <version 10>. Newer NUT reads the last five model
 * columns as a runtime, so the model is kept to five characters. */
static const char IDENT[] = "#EcoFlow         D2Max      PC-" CANARY_VERSION "  \r";
_Static_assert(sizeof(IDENT) == 40, "the I reply must be exactly 39 bytes");

static unsigned decivolts(uint32_t mv)
{
    uint32_t dv = (mv + 50u) / 100u;
    return dv > 9999u ? 9999u : (unsigned)dv;
}

size_t megatec_reply(const char *cmd, const power_view_t *view, const megatec_rated_t *rated, char *out,
                     size_t cap)
{
    int n;

    if (strcmp(cmd, "Q1") == 0) {
        if (view == NULL) {
            return 0;
        }
        unsigned in = decivolts(view->ac_in_mv);
        unsigned outv = decivolts(view->ac_out_mv);
        unsigned load = rated->watts ? (unsigned)view->out_watts * 100u / rated->watts : 0u;
        unsigned batt = BATT_LOW_DV + ((BATT_HIGH_DV - BATT_LOW_DV) * view->soc + 50u) / 100u;
        unsigned hz = view->ac_in_hz > 99 ? 99u : view->ac_in_hz;
        if (load > 999u) {
            load = 999u;
        }
        /* (in fault out load freq batt temp b7..b0
         * b7 utility fail, b6 battery low, b3 standby (offline) type. The
         * fault voltage repeats the input voltage and there is no temperature
         * sensor, so that field is zero. */
        n = snprintf(out, cap, "(%03u.%u %03u.%u %03u.%u %03u %02u.0 %02u.%u 00.0 %c%c001000\r", in / 10,
                     in % 10, in / 10, in % 10, outv / 10, outv % 10, load, hz, batt / 10, batt % 10,
                     view->on_battery ? '1' : '0', view->low_battery ? '1' : '0');
    } else if (strcmp(cmd, "F") == 0) {
        unsigned amps = rated->volts ? rated->watts / rated->volts : 0u;
        n = snprintf(out, cap, "#%03u.0 %03u %03u.%u %02u.0\r", (unsigned)rated->volts, amps > 999u ? 999u : amps,
                     BATT_NOMINAL_DV / 10u, BATT_NOMINAL_DV % 10u, (unsigned)rated->hz);
    } else if (strcmp(cmd, "I") == 0) {
        n = snprintf(out, cap, "%s", IDENT);
    } else {
        n = snprintf(out, cap, "%s\r", cmd);
    }
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
