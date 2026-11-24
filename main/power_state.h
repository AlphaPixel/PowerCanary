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
    uint32_t ac_present_min_mv; /* AC input at or above this counts as mains present */
    uint32_t debounce_ms;       /* AC must be absent this long before "on battery" */
    uint32_t stale_ms;          /* a sample older than this is not reported at all */
    uint8_t low_batt_pct;       /* state of charge at or below this is "battery low" */
} power_cfg_t;

typedef struct {
    int64_t ac_at_ms;      /* time of the last AC sample; negative = never */
    int64_t soc_at_ms;     /* time of the last state-of-charge sample; negative = never */
    int64_t ac_lost_at_ms; /* start of the current run of AC-absent samples; negative = present */
    uint32_t ac_in_mv;
    uint32_t ac_out_mv;
    uint16_t out_watts;
    uint8_t ac_in_hz;
    uint8_t soc;
} power_state_t;

typedef struct {
    bool on_battery;
    bool low_battery;
    uint32_t ac_in_mv;
    uint32_t ac_out_mv;
    uint16_t out_watts;
    uint8_t ac_in_hz;
    uint8_t soc;
} power_view_t;

void power_state_init(power_state_t *s);

void power_state_set_ac(power_state_t *s, const power_cfg_t *c, int64_t now_ms, uint32_t ac_in_mv,
                        uint32_t ac_out_mv, uint16_t out_watts, uint8_t ac_in_hz);

void power_state_set_soc(power_state_t *s, int64_t now_ms, uint8_t soc);

/* Fills *v and returns true only when both the AC sample and the state of
 * charge are fresh. On false the caller must not report any power status. */
bool power_state_view(const power_state_t *s, const power_cfg_t *c, int64_t now_ms, power_view_t *v);
