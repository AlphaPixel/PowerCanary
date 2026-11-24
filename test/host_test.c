/* SPDX-License-Identifier: MIT
 *
 * Host-side check of the platform-independent modules. Build and run:
 *   cmake -S test -B build-host && cmake --build build-host && ctest --test-dir build-host
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ef_proto.h"
#include "megatec.h"
#include "power_state.h"

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

static const power_cfg_t CFG = {
    .ac_present_min_mv = 50000, .debounce_ms = 10000, .stale_ms = 60000, .low_batt_pct = 30};
static const megatec_rated_t RATED = {.volts = 120, .watts = 2400, .hz = 60};

/* Mains present and mains lost as a real DELTA 2 Max reports them. When its AC
 * cord is pulled the voltage reading stays where it was; only the input
 * current and power drop to zero. */
static const power_sample_t MAINS = {.ac_in_mv = 118437, .ac_in_ma = 1105, .in_watts = 116,
                                     .ac_out_mv = 118109, .out_watts = 116, .ac_in_hz = 60};
static const power_sample_t LOST = {.ac_in_mv = 118455, .ac_in_ma = 0, .in_watts = 0,
                                    .ac_out_mv = 119456, .out_watts = 116, .ac_in_hz = 60};

static void ac(power_state_t *s, int64_t now_ms, const power_sample_t *sample)
{
    power_state_set_ac(s, &CFG, now_ms, sample);
}

static void test_power_state(void)
{
    power_state_t s;
    power_view_t v;
    power_state_init(&s);

    /* What counts as mains present: a plausible voltage and power flowing in.
     * A voltage reading with no current and no power is not evidence. */
    power_sample_t x = MAINS;
    CHECK(power_sample_ac_present(&CFG, &MAINS) && !power_sample_ac_present(&CFG, &LOST));
    x.in_watts = 0;
    CHECK(power_sample_ac_present(&CFG, &x)); /* current alone is enough */
    x.ac_in_ma = 0;
    CHECK(!power_sample_ac_present(&CFG, &x));
    x = MAINS;
    x.ac_in_mv = 49999;
    CHECK(!power_sample_ac_present(&CFG, &x)); /* below the voltage floor */

    /* No data yet: nothing may be reported. */
    CHECK(!power_state_view(&s, &CFG, 0, &v));
    /* One of the two samples is not enough. */
    ac(&s, 1000, &MAINS);
    CHECK(!power_state_view(&s, &CFG, 1000, &v));
    power_state_set_soc(&s, 1000, 80);
    CHECK(power_state_view(&s, &CFG, 1000, &v));
    CHECK(v.ac_present && !v.on_battery && !v.low_battery && v.soc == 80 && v.ac_in_mv == 118437);

    /* Mains lost with the voltage reading frozen: still on line inside the
     * debounce window, on battery after. The input is shown as 0 V at once. */
    ac(&s, 2000, &LOST);
    power_state_set_soc(&s, 2000, 80);
    CHECK(power_state_view(&s, &CFG, 2000, &v) && !v.on_battery);
    CHECK(!v.ac_present && v.ac_in_mv == 0 && v.ac_in_hz == 0 && v.ac_out_mv == 119456);
    ac(&s, 11999, &LOST);
    CHECK(power_state_view(&s, &CFG, 11999, &v) && !v.on_battery);
    ac(&s, 12000, &LOST);
    CHECK(power_state_view(&s, &CFG, 12000, &v) && v.on_battery && !v.low_battery);

    /* Low battery at the threshold, not above it. */
    power_state_set_soc(&s, 12000, 31);
    CHECK(power_state_view(&s, &CFG, 12000, &v) && !v.low_battery);
    power_state_set_soc(&s, 12000, 30);
    CHECK(power_state_view(&s, &CFG, 12000, &v) && v.on_battery && v.low_battery);

    /* Stale: no answer at all, whatever the last status was. */
    CHECK(power_state_view(&s, &CFG, 72000, &v));
    CHECK(!power_state_view(&s, &CFG, 72001, &v));

    /* A gap in samples does not restart the outage: still absent means still
     * on battery, with no second debounce. */
    ac(&s, 200000, &LOST);
    power_state_set_soc(&s, 200000, 29);
    CHECK(power_state_view(&s, &CFG, 200000, &v) && v.on_battery && v.low_battery);

    /* Mains back: on line at once; low battery stays true until recharged. */
    ac(&s, 201000, &MAINS);
    CHECK(power_state_view(&s, &CFG, 201000, &v) && v.ac_present && !v.on_battery && v.low_battery);

    /* A brief dip shorter than the debounce never reports on battery. */
    ac(&s, 202000, &LOST);
    ac(&s, 205000, &MAINS);
    ac(&s, 206000, &LOST);
    power_state_set_soc(&s, 215000, 29);
    ac(&s, 215000, &LOST);
    CHECK(power_state_view(&s, &CFG, 215000, &v) && !v.on_battery);
    ac(&s, 216000, &LOST);
    CHECK(power_state_view(&s, &CFG, 216000, &v) && v.on_battery);
}

/* Reads columns [from, to] of a reply as nutdrv_qx does. */
static double col(const char *reply, int from, int to)
{
    char tmp[16] = {0};
    memcpy(tmp, reply + from, (size_t)(to - from + 1));
    return strtod(tmp, NULL);
}

static void test_megatec(void)
{
    char out[MEGATEC_REPLY_MAX];
    power_view_t v = {.on_battery = false, .low_battery = false, .ac_in_mv = 121340, .ac_out_mv = 120010,
                      .out_watts = 312, .ac_in_hz = 60, .soc = 100};

    /* Q1: 47 bytes, fixed columns, CR terminated. */
    CHECK(megatec_reply("Q1", &v, &RATED, out, sizeof out) == 47);
    CHECK(strcmp(out, "(121.3 121.3 120.0 013 60.0 52.0 00.0 00001000\r") == 0);
    CHECK(col(out, 1, 5) == 121.3 && col(out, 13, 17) == 120.0 && col(out, 19, 21) == 13);
    CHECK(col(out, 23, 26) == 60.0 && col(out, 28, 31) == 52.0);
    CHECK(out[38] == '0' && out[39] == '0' && out[46] == '\r');

    v = (power_view_t){.on_battery = true, .low_battery = true, .ac_in_mv = 0, .ac_out_mv = 119960,
                       .out_watts = 2400, .ac_in_hz = 0, .soc = 0};
    CHECK(megatec_reply("Q1", &v, &RATED, out, sizeof out) == 47);
    CHECK(strcmp(out, "(000.0 000.0 120.0 100 00.0 41.6 00.0 11001000\r") == 0);
    CHECK(out[38] == '1' && out[39] == '1');

    /* Out-of-range inputs must never change the reply length. */
    v = (power_view_t){.ac_in_mv = 4000000000u, .ac_out_mv = 4000000000u, .out_watts = 65535, .ac_in_hz = 255,
                       .soc = 100};
    CHECK(megatec_reply("Q1", &v, &RATED, out, sizeof out) == 47);

    /* No fresh data: Q1 stays silent, the static replies still answer. */
    CHECK(megatec_reply("Q1", NULL, &RATED, out, sizeof out) == 0);
    CHECK(megatec_reply("F", NULL, &RATED, out, sizeof out) == 22);
    CHECK(strcmp(out, "#120.0 020 048.0 60.0\r") == 0);
    CHECK(megatec_reply("I", NULL, &RATED, out, sizeof out) == 39);
    CHECK(out[0] == '#' && out[38] == '\r');
    CHECK(memcmp(out + 1, "EcoFlow        ", 15) == 0 && out[16] == ' ');
    CHECK(memcmp(out + 17, "D2Max     ", 10) == 0 && out[27] == ' ');
    CHECK(memcmp(out + 28, "PC-0.1.0  ", 10) == 0);

    /* Unimplemented commands are echoed back. */
    CHECK(megatec_reply("S03", &v, &RATED, out, sizeof out) == 4 && strcmp(out, "S03\r") == 0);
    CHECK(megatec_reply("T", NULL, &RATED, out, sizeof out) == 2 && strcmp(out, "T\r") == 0);

    /* battery.charge as nutdrv_qx derives it must track the state of charge:
     * nominal comes from F, low = nominal*104/120, high = nominal*130/120. */
    megatec_reply("F", NULL, &RATED, out, sizeof out);
    double nom = col(out, 11, 15), low = 104 * nom / 120, high = 130 * nom / 120;
    for (unsigned soc = 0; soc <= 100; soc++) {
        v = (power_view_t){.ac_in_mv = 120000, .ac_out_mv = 120000, .ac_in_hz = 60, .soc = (uint8_t)soc};
        megatec_reply("Q1", &v, &RATED, out, sizeof out);
        double charge = 100 * (col(out, 28, 31) - low) / (high - low);
        CHECK(charge > soc - 1.0 && charge < soc + 1.0);
    }
}

static size_t unhex(const char *hex, uint8_t *out)
{
    size_t n = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        const char pair[3] = {hex[0], hex[1], '\0'};
        out[n++] = (uint8_t)strtoul(pair, NULL, 16);
    }
    return n;
}

/* The firmware's path from a decoded heartbeat to the presence decision. */
static bool present(const ef_inverter_t *iv)
{
    const power_sample_t sample = {.ac_in_mv = iv->ac_in_mv, .ac_in_ma = iv->ac_in_ma, .in_watts = iv->in_watts,
                                   .ac_out_mv = iv->ac_out_mv, .out_watts = iv->out_watts, .ac_in_hz = iv->ac_in_hz};
    return power_sample_ac_present(&CFG, &sample);
}

static void test_ef_proto(void)
{
    /* Published check values for both checksums. */
    CHECK(ef_crc16((const uint8_t *)"123456789", 9) == 0xBB3D);
    CHECK(ef_crc8((const uint8_t *)"123456789", 9) == 0xF4);

    /* Packet round trip, with the header layout the reference documents. */
    uint8_t pkt[64], frame[96], rx[256];
    const uint8_t pay[] = {1, 2, 3, 4, 5};
    size_t pn = ef_build_packet(pkt, sizeof pkt, 0x21, 0x35, 0x35, 0x86, pay, sizeof pay);
    CHECK(pn == sizeof pay + EF_PACKET_OVERHEAD);
    CHECK(pkt[0] == 0xAA && pkt[1] == 0x02 && pkt[2] == 5 && pkt[3] == 0 && pkt[5] == 0x0D);
    CHECK(pkt[12] == 0x21 && pkt[13] == 0x35 && pkt[14] == 0x35 && pkt[15] == 0x86);
    ef_packet_t p;
    CHECK(ef_parse_packet(pkt, pn, &p));
    CHECK(p.src == 0x21 && p.dst == 0x35 && p.cmd_set == 0x35 && p.cmd_id == 0x86);
    CHECK(p.plen == 5 && memcmp(p.payload, pay, 5) == 0 && p.total == pn);
    /* Trailing padding after the packet is tolerated; corruption is not. */
    memset(pkt + pn, 0x0B, 11);
    CHECK(ef_parse_packet(pkt, pn + 11, &p) && p.total == pn);
    pkt[17] ^= 0x01;
    CHECK(!ef_parse_packet(pkt, pn, &p));
    pkt[17] ^= 0x01;
    CHECK(!ef_parse_packet(pkt, pn - 1, &p));

    /* Frame round trip: garbage before, delivery split across notifications. */
    size_t fn = ef_build_frame(frame, sizeof frame, EF_FRAME_PROTOCOL, pkt, pn);
    CHECK(fn == pn + EF_FRAME_OVERHEAD && frame[0] == 0x5A && frame[1] == 0x5A && frame[2] == 0x10);
    const uint8_t *body;
    size_t blen, used;
    memcpy(rx, "\x00\x5A\x11", 3);
    memcpy(rx + 3, frame, fn);
    CHECK(!ef_next_frame(rx, 3 + fn - 1, &body, &blen, &used) && used == 3);
    CHECK(ef_next_frame(rx, 3 + fn, &body, &blen, &used));
    CHECK(used == 3 + fn && blen == pn && memcmp(body, pkt, pn) == 0);
    /* A lone trailing 5A is kept, other garbage is dropped. */
    CHECK(!ef_next_frame((const uint8_t *)"\x01\x02\x5A", 3, &body, &blen, &used) && used == 2);
    CHECK(!ef_next_frame((const uint8_t *)"\x01\x02\x03", 3, &body, &blen, &used) && used == 3);
    /* A corrupted frame is skipped and the next good one is found. */
    memcpy(rx, frame, fn);
    rx[10] ^= 0xFF;
    memcpy(rx + fn, frame, fn);
    CHECK(ef_next_frame(rx, 2 * fn, &body, &blen, &used) && used == 2 * fn && blen == pn);

    /* Inverter heartbeat: pack the reference struct field by field
     * (<IIBHHBIIBIIB...) so the offsets are derived independently. */
    uint8_t inv[40] = {0};
    size_t o = 0;
    o += 4;                                  /* errCode      I */
    o += 4;                                  /* sysVer       I */
    o += 1;                                  /* chargerType  B */
    o += 2;                                  /* inputWatts   H */
    inv[o] = 0x38, inv[o + 1] = 0x01, o += 2; /* outputWatts  H = 312 */
    o += 1;                                  /* invType      B */
    inv[o] = 0xCA, inv[o + 1] = 0xD4, inv[o + 2] = 0x01, o += 4; /* invOutVol I = 120010 */
    o += 4;                                  /* invOutAmp    I */
    o += 1;                                  /* invOutFreq   B */
    inv[o] = 0xFC, inv[o + 1] = 0xD9, inv[o + 2] = 0x01, o += 4; /* acInVol   I = 121340 */
    o += 4;                                  /* acInAmp      I */
    inv[o] = 60, o += 1;                     /* acInFreq     B */
    CHECK(o == 32);
    ef_inverter_t iv;
    CHECK(ef_decode_inverter(inv, sizeof inv, &iv));
    CHECK(iv.out_watts == 312 && iv.ac_out_mv == 120010 && iv.ac_in_mv == 121340 && iv.ac_in_hz == 60);
    CHECK(iv.in_watts == 0 && iv.ac_in_ma == 0);
    CHECK(!ef_decode_inverter(inv, 31, &iv));

    /* Two inverter heartbeats captured from a real DELTA 2 Max: on mains, and
     * a quarter of an hour after its AC cord was pulled. The voltage reading
     * barely moves; input power and current are what show the outage. */
    uint8_t real[40];
    CHECK(unhex("000000005b000002ff74007400005dcd0100510400003ca5ce0100510400003c1e00000000000000", real) == 40);
    CHECK(ef_decode_inverter(real, sizeof real, &iv));
    CHECK(iv.in_watts == 116 && iv.ac_in_ma == 1105 && iv.ac_in_mv == 118437 && iv.ac_in_hz == 60);
    CHECK(iv.out_watts == 116 && iv.ac_out_mv == 118109);
    CHECK(present(&iv));
    CHECK(unhex("000000005b000002000000740000a0d201006d0400003cb7ce0100000000003c2700000000000000", real) == 40);
    CHECK(ef_decode_inverter(real, sizeof real, &iv));
    CHECK(iv.in_watts == 0 && iv.ac_in_ma == 0 && iv.ac_in_mv == 118455 && iv.ac_in_hz == 60);
    CHECK(iv.out_watts == 116 && iv.ac_out_mv == 119456);
    CHECK(!present(&iv));

    /* EMS heartbeat: <BBBIIBBBB..., lcdShowSoc is the ninth field. */
    uint8_t ems[32] = {0}, soc = 0;
    o = 1 + 1 + 1 + 4 + 4 + 1 + 1 + 1;
    CHECK(o == 14);
    ems[o] = 87;
    CHECK(ef_decode_soc(ems, sizeof ems, &soc) && soc == 87);
    CHECK(!ef_decode_soc(ems, 14, &soc));

    /* Routing. */
    p = (ef_packet_t){.src = 0x04, .cmd_set = 0x20, .cmd_id = 0x02};
    CHECK(ef_is_inverter_heartbeat(&p) && !ef_is_ems_heartbeat(&p));
    p = (ef_packet_t){.src = 0x03, .cmd_set = 0x20, .cmd_id = 0x02};
    CHECK(ef_is_ems_heartbeat(&p) && !ef_is_inverter_heartbeat(&p));
}

int main(void)
{
    test_power_state();
    test_megatec();
    test_ef_proto();
    puts("host_test: all checks passed");
    return 0;
}
