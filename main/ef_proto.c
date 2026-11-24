/* SPDX-License-Identifier: Apache-2.0
 * Derived from rabits/ha-ef-ble and kernelorg/esphome-ecoflow-ble; see ef_proto.h. */
#include "ef_proto.h"

#include <string.h>

/* crc8: CCITT, polynomial 0x07. crc16: ARC, reflected polynomial 0xA001. */
uint8_t ef_crc8(const uint8_t *d, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
        }
    }
    return c;
}

uint16_t ef_crc16(const uint8_t *d, size_t n)
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
        }
    }
    return c;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

size_t ef_build_frame(uint8_t *out, size_t cap, uint8_t type, const uint8_t *body, size_t blen)
{
    size_t total = blen + EF_FRAME_OVERHEAD;
    if (total > cap || blen > 0xFFFF - 2) {
        return 0;
    }
    out[0] = 0x5A;
    out[1] = 0x5A;
    out[2] = type;
    out[3] = 0x01;
    put16(out + 4, (uint16_t)(blen + 2)); /* the length counts the checksum */
    memcpy(out + 6, body, blen);
    put16(out + 6 + blen, ef_crc16(out, 6 + blen));
    return total;
}

size_t ef_build_packet(uint8_t *out, size_t cap, uint8_t src, uint8_t dst, uint8_t cmd_set, uint8_t cmd_id,
                       const uint8_t *payload, size_t plen)
{
    size_t total = plen + EF_PACKET_OVERHEAD;
    if (total > cap || plen > 0xFFFF) {
        return 0;
    }
    memset(out, 0, 16);
    out[0] = 0xAA;
    out[1] = 0x02;
    put16(out + 2, (uint16_t)plen);
    out[4] = ef_crc8(out, 4);
    out[5] = 0x0D; /* bytes 6..9 sequence and 10..11 reserved stay zero */
    out[12] = src;
    out[13] = dst;
    out[14] = cmd_set;
    out[15] = cmd_id;
    if (plen) {
        memcpy(out + 16, payload, plen);
    }
    put16(out + 16 + plen, ef_crc16(out, 16 + plen));
    return total;
}

bool ef_next_frame(const uint8_t *buf, size_t len, const uint8_t **body, size_t *blen, size_t *consumed)
{
    size_t i = 0;
    while (len - i >= 2) {
        if (buf[i] != 0x5A || buf[i + 1] != 0x5A) {
            i++;
            continue;
        }
        if (len - i < 6) {
            break; /* header not complete yet */
        }
        size_t flen = 6u + le16(buf + i + 4);
        if (flen < EF_FRAME_OVERHEAD || flen > EF_FRAME_MAX) {
            i += 2; /* not a real header */
            continue;
        }
        if (len - i < flen) {
            break; /* body not complete yet */
        }
        if (ef_crc16(buf + i, flen - 2) != le16(buf + i + flen - 2)) {
            i += 2;
            continue;
        }
        *body = buf + i + 6;
        *blen = flen - EF_FRAME_OVERHEAD;
        *consumed = i + flen;
        return true;
    }
    /* Keep a lone trailing 0x5A: it may be the first half of a frame marker. */
    *consumed = (len - i == 1 && buf[i] != 0x5A) ? len : i;
    return false;
}

bool ef_parse_packet(const uint8_t *d, size_t n, ef_packet_t *p)
{
    if (n < EF_PACKET_OVERHEAD || d[0] != 0xAA || ef_crc8(d, 4) != d[4]) {
        return false;
    }
    size_t start;
    if (d[1] == 0x02) {
        start = 16;
    } else if (d[1] == 0x03) {
        start = 18; /* version 3 adds two routing bytes after dst */
    } else {
        return false;
    }
    uint16_t plen = le16(d + 2);
    size_t total = start + plen + 2;
    if (n < total || ef_crc16(d, total - 2) != le16(d + total - 2)) {
        return false;
    }
    p->src = d[12];
    p->dst = d[13];
    p->cmd_set = d[start - 2];
    p->cmd_id = d[start - 1];
    p->payload = d + start;
    p->plen = plen;
    p->total = total;
    return true;
}

/* Heartbeat routing and layouts follow ha-ef-ble's Delta2Base and its
 * DirectInvDeltaHeartbeatPack / DirectEmsDeltaHeartbeatPack: packed,
 * little-endian, fixed offsets. */
bool ef_is_inverter_heartbeat(const ef_packet_t *p)
{
    return p->src == 0x04 && p->cmd_id == 0x02;
}

bool ef_is_ems_heartbeat(const ef_packet_t *p)
{
    return p->src == 0x03 && p->cmd_set == 0x20 && p->cmd_id == 0x02;
}

bool ef_decode_inverter(const uint8_t *payload, size_t n, ef_inverter_t *o)
{
    if (n < 32) {
        return false;
    }
    o->out_watts = le16(payload + 11); /* outputWatts */
    o->ac_out_mv = le32(payload + 14); /* invOutVol */
    o->ac_in_mv = le32(payload + 23);  /* acInVol */
    o->ac_in_hz = payload[31];         /* acInFreq */
    return true;
}

bool ef_decode_soc(const uint8_t *payload, size_t n, uint8_t *soc)
{
    if (n < 15) {
        return false;
    }
    *soc = payload[14]; /* lcdShowSoc */
    return true;
}
