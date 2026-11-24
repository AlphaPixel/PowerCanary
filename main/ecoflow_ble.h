/* SPDX-License-Identifier: Apache-2.0
 *
 * Read-only Bluetooth LE client for an EcoFlow DELTA 2 family power station:
 * finds the unit by serial number, logs in, and reports the inverter and
 * state-of-charge heartbeats. It never writes a setting to the unit.
 *
 * Derived from rabits/ha-ef-ble and kernelorg/esphome-ecoflow-ble, both
 * Apache-2.0; rewritten in C for ESP-IDF and NimBLE.
 */
#pragma once

#include <stdint.h>

#include "ef_proto.h"

/* Both callbacks run on the Bluetooth host task: keep them short. */
typedef void (*ecoflow_inverter_cb)(const ef_inverter_t *inv);
typedef void (*ecoflow_soc_cb)(uint8_t soc);

enum {
    ECOFLOW_IDLE,       /* waiting out a reconnect delay */
    ECOFLOW_SCANNING,   /* looking for the configured serial number */
    ECOFLOW_CONNECTING, /* link being established */
    ECOFLOW_HANDSHAKE,  /* discovery, key exchange and login in progress */
    ECOFLOW_ONLINE,     /* logged in, heartbeats expected */
    ECOFLOW_CLOSING,    /* dropping the link after an error */
};

enum {
    ECOFLOW_ERR_NONE,
    ECOFLOW_ERR_ENCRYPTION, /* the unit advertises an encryption type other than 7 */
    ECOFLOW_ERR_GATT,       /* characteristics or descriptor not found, or a write failed */
    ECOFLOW_ERR_KEY,        /* key exchange failed */
    ECOFLOW_ERR_LOGIN,      /* the unit rejected the user ID */
    ECOFLOW_ERR_TIMEOUT,    /* handshake or heartbeats timed out */
};

typedef struct {
    uint8_t state;
    uint8_t step;         /* handshake step reached: 0 discovery, 1 key sent, 2 key info, 3 status, 4 login */
    uint8_t encrypt_type; /* as advertised by the unit */
    uint8_t last_error;
    uint8_t last_error_detail; /* e.g. the login result code from the unit */
    int8_t rssi;               /* dBm; from the advertisement, then from the link */
    uint32_t adverts;          /* advertisements seen from any device: zero means a radio or antenna problem */
    uint32_t units_seen;       /* EcoFlow advertisements seen, any serial: tells range from a wrong serial */
    uint32_t sessions;         /* successful logins since boot */
    uint32_t drops;            /* links lost since boot */
    uint32_t bad_packets;      /* frames that decrypted to an invalid packet */
    uint32_t inverter_packets; /* inverter heartbeats decoded */
    uint32_t ems_packets;      /* state-of-charge heartbeats decoded */
} ecoflow_ble_status_t;

/* Starts the Bluetooth stack and the connect loop. serial is the unit's
 * 16-character serial number; both strings must stay valid. NVS must be
 * initialised first. */
void ecoflow_ble_start(const char *serial, const char *user_id, ecoflow_inverter_cb on_inverter,
                       ecoflow_soc_cb on_soc);

/* Snapshot for diagnostics. May be called from any task. */
void ecoflow_ble_status(ecoflow_ble_status_t *out);

/* Test builds only: the start of the last inverter and state-of-charge
 * payloads as hex, for checking field offsets against a real unit. Returns the
 * length written. */
size_t ecoflow_ble_raw(char *out, size_t cap);
