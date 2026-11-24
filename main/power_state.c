/* SPDX-License-Identifier: MIT */
#include "power_state.h"

bool power_sample_ac_present(const power_cfg_t *c, const power_sample_t *in)
{
    return in->ac_in_mv >= c->ac_present_min_mv && (in->ac_in_ma > 0 || in->in_watts > 0);
}

void power_state_init(power_state_t *s)
{
    *s = (power_state_t){.ac_at_ms = -1, .soc_at_ms = -1, .ac_lost_at_ms = -1};
}

void power_state_set_ac(power_state_t *s, const power_cfg_t *c, int64_t now_ms, const power_sample_t *in)
{
    s->ac_at_ms = now_ms;
    s->ac = *in;

    if (power_sample_ac_present(c, in)) {
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
    bool present = s->ac_lost_at_ms < 0;
    *v = (power_view_t){
        .on_battery = !present && now_ms - s->ac_lost_at_ms >= (int64_t)c->debounce_ms,
        .low_battery = s->soc <= c->low_batt_pct,
        .ac_present = present,
        /* An absent input is shown as 0 V, not as the stale voltage. */
        .ac_in_mv = present ? s->ac.ac_in_mv : 0,
        .ac_in_hz = present ? s->ac.ac_in_hz : 0,
        .ac_out_mv = s->ac.ac_out_mv,
        .out_watts = s->ac.out_watts,
        .soc = s->soc,
    };
    return true;
}
