/* SPDX-License-Identifier: MIT
 *
 * Megatec/Q1 serial UPS responder, as read by NUT's nutdrv_qx driver
 * (protocol=megatec). Pure C; formats are checked against the NUT source
 * drivers/nutdrv_qx_megatec.c, which reads every field at a fixed column.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "power_state.h"

#define CANARY_VERSION "0.1.0" /* exactly five characters: it fills a fixed-width field */
#define MEGATEC_REPLY_MAX 48   /* longest reply (Q1) is 47 bytes including the CR */

/* Nameplate values reported by the F command. */
typedef struct {
    uint16_t volts; /* nominal AC voltage */
    uint16_t watts; /* rated AC output */
    uint8_t hz;     /* nominal AC frequency */
} megatec_rated_t;

/* Writes the reply to one command line (given without its trailing CR) into
 * out and returns its length; 0 means stay silent.
 *
 * view == NULL means there is no fresh data. Q1 then gets no reply at all, so
 * the driver times out and marks the UPS data stale instead of trusting an
 * old status. Commands this device does not implement are echoed back, which
 * is how a Megatec UPS rejects a command. */
size_t megatec_reply(const char *cmd, const power_view_t *view, const megatec_rated_t *rated, char *out,
                     size_t cap);
