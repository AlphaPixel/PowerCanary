/* SPDX-License-Identifier: Apache-2.0
 *
 * EcoFlow BLE wire format: checksums, frames, packets and the two DELTA 2
 * family heartbeats this project reads. Pure C with no platform dependencies.
 *
 * Derived from rabits/ha-ef-ble and kernelorg/esphome-ecoflow-ble, both
 * Apache-2.0; rewritten in C and reduced to read-only telemetry.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EF_FRAME_COMMAND 0x00  /* plain handshake command */
#define EF_FRAME_PROTOCOL 0x10 /* AES-encrypted packet */
#define EF_FRAME_OVERHEAD 8    /* 5A 5A, type, 01, length (2), ..., crc16 (2) */
#define EF_FRAME_MAX 1024      /* longest frame accepted; also the receive buffer size */
#define EF_PACKET_OVERHEAD 18  /* version 2 header (16) + crc16 (2) */

uint8_t ef_crc8(const uint8_t *d, size_t n);
uint16_t ef_crc16(const uint8_t *d, size_t n);

/* Wraps body in a 5A5A frame. Returns the frame length, 0 if it does not fit. */
size_t ef_build_frame(uint8_t *out, size_t cap, uint8_t type, const uint8_t *body, size_t blen);

/* Builds a version 2 packet. Returns its length, 0 if it does not fit. */
size_t ef_build_packet(uint8_t *out, size_t cap, uint8_t src, uint8_t dst, uint8_t cmd_set, uint8_t cmd_id,
                       const uint8_t *payload, size_t plen);

/* Takes the next complete, checksum-valid frame out of a receive buffer.
 * On true, body/blen describe the frame body (valid until the next call) and
 * *consumed is how many leading bytes the caller must drop afterwards. On
 * false, *consumed is how many leading bytes are garbage and can be dropped
 * now; the rest is an incomplete frame to keep. */
bool ef_next_frame(const uint8_t *buf, size_t len, const uint8_t **body, size_t *blen, size_t *consumed);

typedef struct {
    uint8_t src, dst, cmd_set, cmd_id;
    const uint8_t *payload;
    uint16_t plen;
    size_t total; /* bytes this packet occupies, including its checksum */
} ef_packet_t;

/* Parses one decrypted packet (version 2 or 3) and verifies both checksums. */
bool ef_parse_packet(const uint8_t *d, size_t n, ef_packet_t *p);

typedef struct {
    uint32_t ac_in_mv;
    uint32_t ac_out_mv;
    uint16_t out_watts;
    uint8_t ac_in_hz;
} ef_inverter_t;

bool ef_is_inverter_heartbeat(const ef_packet_t *p);
bool ef_is_ems_heartbeat(const ef_packet_t *p);
bool ef_decode_inverter(const uint8_t *payload, size_t n, ef_inverter_t *o);
bool ef_decode_soc(const uint8_t *payload, size_t n, uint8_t *soc);
