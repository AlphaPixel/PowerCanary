/* SPDX-License-Identifier: MIT */
#include "power_state.h"

void power_state_init(power_state_t *s)
{
    *s = (power_state_t){.ac_at_ms = -1, .soc_at_ms = -1, .ac_lost_at_ms = -1};
}

void power_state_set_ac(power_state_t *s, const power_cfg_t *c, int64_t now_ms, uint32_t ac_in_mv,
                        uint32_t ac_out_mv, uint16_t out_watts, uint8_t ac_in_hz)
{
    s->ac_at_ms = now_ms;
    s->ac_in_mv = ac_in_mv;
    s->ac_out_mv = ac_out_mv;
    s->out_watts = out_watts;
    s->ac_in_hz = ac_in_hz;

    if (ac_in_mv >= c->ac_present_min_mv) {
        s->ac_lost_at_ms = -1;
    } else if (s->ac_lost_at_ms < 0) {
        /* First absent sample of this run. A gap in samples does not restart
         * the run: only a sample showing mains present does. */
        s->ac_lost_at_ms = now_ms;
    }
}

void power_state_set_soc(power_state_t *s, int64_t now_ms, uint8_t soc)
{
    s->soc_at_ms = now_ms;
    s->soc = soc > 100 ? 100 : soc;
}

static bool fresh(int64_t at_ms, int64_t now_ms, uint32_t stale_ms)
{
    return at_ms >= 0 && now_ms - at_ms <= (int64_t)stale_ms;
}

bool power_state_view(const power_state_t *s, const power_cfg_t *c, int64_t now_ms, power_view_t *v)
{
    if (!fresh(s->ac_at_ms, now_ms, c->stale_ms) || !fresh(s->soc_at_ms, now_ms, c->stale_ms)) {
        return false;
    }
    *v = (power_view_t){
        .on_battery = s->ac_lost_at_ms >= 0 && now_ms - s->ac_lost_at_ms >= (int64_t)c->debounce_ms,
        .low_battery = s->soc <= c->low_batt_pct,
        .ac_in_mv = s->ac_in_mv,
        .ac_out_mv = s->ac_out_mv,
        .out_watts = s->out_watts,
        .ac_in_hz = s->ac_in_hz,
        .soc = s->soc,
    };
    return true;
}
