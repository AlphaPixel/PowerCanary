/* SPDX-License-Identifier: MIT
 *
 * Safety logic: turns raw power-station samples into the two facts a UPS
 * driver acts on (on battery, battery low), or into "no answer" when the data
 * is not fresh. Pure C with no platform dependencies; the caller passes the
 * time in, so the rules can be tested on a host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t ac_present_min_mv; /* AC input below this never counts as mains present */
    uint32_t debounce_ms;       /* AC must be absent this long before "on battery" */
    uint32_t stale_ms;          /* a sample older than this is not reported at all */
    uint8_t low_batt_pct;       /* state of charge at or below this is "battery low" */
} power_cfg_t;

/* One inverter reading. */
typedef struct {
    uint32_t ac_in_mv; /* AC input voltage; see power_sample_ac_present() */
    uint32_t ac_in_ma; /* AC input current */
    uint16_t in_watts; /* AC input power */
    uint32_t ac_out_mv;
    uint16_t out_watts;
    uint8_t ac_in_hz;
} power_sample_t;

typedef struct {
    int64_t ac_at_ms;      /* time of the last AC sample; negative = never */
    int64_t soc_at_ms;     /* time of the last state-of-charge sample; negative = never */
    int64_t ac_lost_at_ms; /* start of the current run of AC-absent samples; negative = present */
    power_sample_t ac;
    uint8_t soc;
} power_state_t;

typedef struct {
    bool on_battery;
    bool low_battery;
    bool ac_present;   /* this instant, before the debounce */
    uint32_t ac_in_mv; /* zero while AC is absent */
    uint8_t ac_in_hz;  /* zero while AC is absent */
    uint32_t ac_out_mv;
    uint16_t out_watts;
    uint8_t soc;
} power_view_t;

/* Mains counts as present only when the input voltage is plausible AND power
 * is actually flowing in. Voltage alone is not evidence: a DELTA 2 Max keeps
 * reporting the last voltage it measured after its AC cord is pulled, while
 * its input current and power drop to zero. */
bool power_sample_ac_present(const power_cfg_t *c, const power_sample_t *in);

void power_state_init(power_state_t *s);
void power_state_set_ac(power_state_t *s, const power_cfg_t *c, int64_t now_ms, const power_sample_t *in);
void power_state_set_soc(power_state_t *s, int64_t now_ms, uint8_t soc);

/* Fills *v and returns true only when both the AC sample and the state of
 * charge are fresh. On false the caller must not report any power status. */
bool power_state_view(const power_state_t *s, const power_cfg_t *c, int64_t now_ms, power_view_t *v);
