/* SPDX-License-Identifier: Apache-2.0
 * Derived from rabits/ha-ef-ble and kernelorg/esphome-ecoflow-ble; see ecoflow_ble.h.
 *
 * Everything below the public functions runs on the NimBLE host task, so the
 * state machine needs no locking.
 */
#include "ecoflow_ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "uECC.h"

static const char *TAG = "ecoflow";

#define SERIAL_LEN 16
#define COMPANY_ID 0xB5B5
#define AUTH_MODULE 0x35      /* address of the unit's login module; also its command set */
#define HANDSHAKE_MS 20000    /* connect to logged in */
#define DATA_TIMEOUT_MS 30000 /* longest silence tolerated once logged in */
#define BACKOFF_MIN_MS 2000
#define BACKOFF_MAX_MS 60000

/* Session key table from the reference implementation, embedded as a binary. */
extern const uint8_t keydata_start[] asm("_binary_keydata_bin_start");
extern const uint8_t keydata_end[] asm("_binary_keydata_bin_end");

/* The unit exposes its serial stream under one of two characteristic pairs. */
static const ble_uuid16_t UUID_WRITE_16 = BLE_UUID16_INIT(0x0002);
static const ble_uuid16_t UUID_NOTIFY_16 = BLE_UUID16_INIT(0x0003);
static const ble_uuid128_t UUID_WRITE_NUS = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93,
                                                            0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_NOTIFY_NUS = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93,
                                                             0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);
static const ble_uuid16_t UUID_CCCD = BLE_UUID16_INIT(0x2902);

/* Handshake steps inside ECOFLOW_HANDSHAKE. */
enum { STEP_DISCOVER, STEP_PUBKEY, STEP_KEYINFO, STEP_AUTH_STATUS, STEP_AUTH };

typedef struct {
    uint16_t write, notify, notify_end;
    uint8_t write_props;
} gatt_pair_t;

static struct {
    const char *serial;
    const char *user_id;
    ecoflow_inverter_cb on_inverter;
    ecoflow_soc_cb on_soc;

    uint8_t own_addr_type;
    uint16_t conn;
    gatt_pair_t pair[2]; /* [0] 16-bit pair, preferred; [1] Nordic UART pair */
    gatt_pair_t gatt;    /* the pair in use */
    uint16_t cccd;

    uint8_t priv[21]; /* secp160r1 private keys are 21 bytes in micro-ecc */
    uint8_t pub[40];
    uint8_t key[16], iv[16];
    bool have_session;

    uint8_t rx[EF_FRAME_MAX];
    size_t rx_len;

    int64_t deadline_ms; /* 0 = none */
    int64_t retry_at_ms;
    uint32_t backoff_ms;
    struct ble_npl_callout tick;
    ecoflow_ble_status_t status;
#if CONFIG_CANARY_TEST_COMMANDS
    uint8_t raw_inv[40], raw_ems[32];
    uint8_t raw_inv_len, raw_ems_len;
#endif
} s;

static int gap_event(struct ble_gap_event *event, void *arg);

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int rng(uint8_t *dest, unsigned size)
{
    esp_fill_random(dest, size); /* true random while the radio is on */
    return 1;
}

static void aes_cbc(int mode, const uint8_t *in, size_t len, uint8_t *out)
{
    mbedtls_aes_context ctx;
    uint8_t iv[16];
    memcpy(iv, s.iv, sizeof iv);
    mbedtls_aes_init(&ctx);
    if (mode == MBEDTLS_AES_ENCRYPT) {
        mbedtls_aes_setkey_enc(&ctx, s.key, 128);
    } else {
        mbedtls_aes_setkey_dec(&ctx, s.key, 128);
    }
    mbedtls_aes_crypt_cbc(&ctx, mode, len, iv, in, out);
    mbedtls_aes_free(&ctx);
}

/* ── link lifecycle ────────────────────────────────────────────────────── */

static void link_down(void)
{
    s.conn = BLE_HS_CONN_HANDLE_NONE;
    s.have_session = false;
    s.rx_len = 0;
    s.deadline_ms = 0;
    memset(s.pair, 0, sizeof s.pair);
    memset(&s.gatt, 0, sizeof s.gatt);
    s.cccd = 0;
    s.retry_at_ms = now_ms() + s.backoff_ms;
    s.backoff_ms = s.backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s.backoff_ms * 2;
    s.status.state = ECOFLOW_IDLE;
}

static void fail(uint8_t error, uint8_t detail, const char *why)
{
    ESP_LOGW(TAG, "%s (detail %u)", why, detail);
    s.status.last_error = error;
    s.status.last_error_detail = detail;
    s.have_session = false;
    s.deadline_ms = 0;
    if (s.conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_terminate(s.conn, BLE_ERR_REM_USER_CONN_TERM) == 0) {
        s.status.state = ECOFLOW_CLOSING; /* the disconnect event finishes the job */
    } else {
        link_down();
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {.passive = 0, .filter_duplicates = 1};
    int rc = ble_gap_disc(s.own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
        s.retry_at_ms = now_ms() + BACKOFF_MIN_MS;
        return;
    }
    s.status.state = ECOFLOW_SCANNING;
}

/* ── sending ───────────────────────────────────────────────────────────── */

static bool write_frame(const uint8_t *frame, size_t len)
{
    int rc = (s.gatt.write_props & BLE_GATT_CHR_PROP_WRITE)
                 ? ble_gattc_write_flat(s.conn, s.gatt.write, frame, (uint16_t)len, NULL, NULL)
                 : ble_gattc_write_no_rsp_flat(s.conn, s.gatt.write, frame, (uint16_t)len);
    if (rc != 0) {
        fail(ECOFLOW_ERR_GATT, (uint8_t)rc, "write failed");
        return false;
    }
    return true;
}

static bool send_command(const uint8_t *body, size_t len)
{
    uint8_t frame[64];
    size_t n = ef_build_frame(frame, sizeof frame, EF_FRAME_COMMAND, body, len);
    return n && write_frame(frame, n);
}

static bool send_packet(uint8_t cmd_id, const uint8_t *payload, size_t plen)
{
    uint8_t inner[80], enc[80], frame[80 + EF_FRAME_OVERHEAD];
    size_t n = ef_build_packet(inner, 64, 0x21, AUTH_MODULE, AUTH_MODULE, cmd_id, payload, plen);
    if (n == 0) {
        return false;
    }
    uint8_t pad = (uint8_t)(16 - n % 16); /* PKCS#7 */
    memset(inner + n, pad, pad);
    n += pad;
    aes_cbc(MBEDTLS_AES_ENCRYPT, inner, n, enc);
    return write_frame(frame, ef_build_frame(frame, sizeof frame, EF_FRAME_PROTOCOL, enc, n));
}

static void send_login(void)
{
    /* The login token is the uppercase hex MD5 of user ID + serial number. */
    char text[64];
    uint8_t digest[16], token[32];
    int n = snprintf(text, sizeof text, "%s%s", s.user_id, s.serial);
    if (n <= 0 || n >= (int)sizeof text) {
        fail(ECOFLOW_ERR_LOGIN, 0, "user ID too long");
        return;
    }
    mbedtls_md5((const uint8_t *)text, (size_t)n, digest);
    for (int i = 0; i < 16; i++) {
        token[i * 2] = (uint8_t) "0123456789ABCDEF"[digest[i] >> 4];
        token[i * 2 + 1] = (uint8_t) "0123456789ABCDEF"[digest[i] & 0x0F];
    }
    if (send_packet(0x86, token, sizeof token)) {
        s.status.step = STEP_AUTH;
    }
}

/* ── receiving ─────────────────────────────────────────────────────────── */

static void on_pubkey(const uint8_t *body, size_t len)
{
    if (len < 43 || body[0] != 0x01) {
        return; /* not the reply to our key; keep waiting */
    }
    if (body[2] >= 1 && body[2] <= 4) {
        fail(ECOFLOW_ERR_KEY, body[2], "unit asked for a key exchange curve other than secp160r1");
        return;
    }
    uint8_t secret[20];
    if (!uECC_valid_public_key(body + 3, uECC_secp160r1()) ||
        !uECC_shared_secret(body + 3, s.priv, secret, uECC_secp160r1())) {
        fail(ECOFLOW_ERR_KEY, 0, "key exchange failed");
        return;
    }
    /* Interim cipher: key = secret[0..15], IV = MD5(secret). */
    mbedtls_md5(secret, sizeof secret, s.iv);
    memcpy(s.key, secret, sizeof s.key);
    const uint8_t get_key_info = 0x02;
    if (send_command(&get_key_info, 1)) {
        s.status.step = STEP_KEYINFO;
    }
}

static void on_keyinfo(const uint8_t *body, size_t len)
{
    if (len < 33 || body[0] != 0x02) {
        return; /* the unit may repeat its public key first */
    }
    /* Decrypts to sRand (16 bytes) then a 2-byte seed selecting 16 bytes of the
     * key table. Session key = MD5(table[pos..pos+15] + sRand). */
    uint8_t dec[32], md5_in[32];
    aes_cbc(MBEDTLS_AES_DECRYPT, body + 1, sizeof dec, dec);
    size_t pos = dec[16] * 0x10u + (uint8_t)(dec[17] - 1) * 0x100u;
    if (pos + 16 > (size_t)(keydata_end - keydata_start)) {
        fail(ECOFLOW_ERR_KEY, 1, "key seed out of range");
        return;
    }
    memcpy(md5_in, keydata_start + pos, 16);
    memcpy(md5_in + 16, dec, 16);
    mbedtls_md5(md5_in, sizeof md5_in, s.key);
    s.have_session = true;
    if (send_packet(0x89, NULL, 0)) { /* ask for the login status */
        s.status.step = STEP_AUTH_STATUS;
    }
}

static void on_packet(const ef_packet_t *p)
{
    bool from_auth = p->src == AUTH_MODULE && p->cmd_set == AUTH_MODULE;

    if (s.status.state == ECOFLOW_HANDSHAKE) {
        if (s.status.step == STEP_AUTH_STATUS) {
            if (from_auth) {
                send_login();
            }
            return;
        }
        if (s.status.step != STEP_AUTH || (from_auth && p->cmd_id != 0x86)) {
            return;
        }
        if (from_auth && p->plen > 0 && p->payload[0] != 0) {
            s.backoff_ms = BACKOFF_MAX_MS; /* wrong credentials will not fix themselves */
            fail(ECOFLOW_ERR_LOGIN, p->payload[0], "login rejected by the unit");
            return;
        }
        /* Confirmed by the login reply, or by the first data packet. */
        ESP_LOGI(TAG, "logged in");
        s.status.state = ECOFLOW_ONLINE;
        s.status.sessions++;
        s.status.last_error = ECOFLOW_ERR_NONE;
        s.status.last_error_detail = 0;
        s.backoff_ms = BACKOFF_MIN_MS;
    }
    if (s.status.state != ECOFLOW_ONLINE) {
        return;
    }
    s.deadline_ms = now_ms() + DATA_TIMEOUT_MS;

    ef_inverter_t inv;
    uint8_t soc;
    if (ef_is_inverter_heartbeat(p) && ef_decode_inverter(p->payload, p->plen, &inv)) {
        s.status.inverter_packets++;
        s.on_inverter(&inv);
    } else if (ef_is_ems_heartbeat(p) && ef_decode_soc(p->payload, p->plen, &soc)) {
        s.status.ems_packets++;
        s.on_soc(soc);
    }
#if CONFIG_CANARY_TEST_COMMANDS
    if (ef_is_inverter_heartbeat(p)) {
        s.raw_inv_len = (uint8_t)(p->plen < sizeof s.raw_inv ? p->plen : sizeof s.raw_inv);
        memcpy(s.raw_inv, p->payload, s.raw_inv_len);
    } else if (ef_is_ems_heartbeat(p)) {
        s.raw_ems_len = (uint8_t)(p->plen < sizeof s.raw_ems ? p->plen : sizeof s.raw_ems);
        memcpy(s.raw_ems, p->payload, s.raw_ems_len);
    }
#endif
}

static void on_frame(const uint8_t *body, size_t len)
{
    if (s.status.state == ECOFLOW_HANDSHAKE && s.status.step == STEP_PUBKEY) {
        on_pubkey(body, len);
        return;
    }
    if (s.status.state == ECOFLOW_HANDSHAKE && s.status.step == STEP_KEYINFO) {
        on_keyinfo(body, len);
        return;
    }
    if (!s.have_session) {
        return;
    }
    /* Whole AES blocks only, as the unit itself does. The packet checksum
     * tells where the packet ends, so the padding needs no parsing. */
    static uint8_t dec[EF_FRAME_MAX];
    size_t aligned = len & ~(size_t)15;
    if (aligned == 0) {
        return;
    }
    aes_cbc(MBEDTLS_AES_DECRYPT, body, aligned, dec);
    ef_packet_t p;
    size_t off = 0;
    while (off < aligned && ef_parse_packet(dec + off, aligned - off, &p)) {
        on_packet(&p);
        off += p.total;
    }
    if (off == 0) {
        s.status.bad_packets++;
    }
}

static void on_notify(struct os_mbuf *om)
{
    size_t len = OS_MBUF_PKTLEN(om);
    if (len > sizeof s.rx - s.rx_len) {
        s.rx_len = 0; /* overflow: resynchronise on the next frame marker */
        if (len > sizeof s.rx) {
            return;
        }
    }
    os_mbuf_copydata(om, 0, (int)len, s.rx + s.rx_len);
    s.rx_len += len;

    for (;;) {
        const uint8_t *body;
        size_t blen, used;
        bool ok = ef_next_frame(s.rx, s.rx_len, &body, &blen, &used);
        if (ok) {
            on_frame(body, blen);
        }
        if (used > s.rx_len) { /* the link was reset while handling the frame */
            break;
        }
        memmove(s.rx, s.rx + used, s.rx_len - used);
        s.rx_len -= used;
        if (!ok) {
            break;
        }
    }
}

/* ── GATT setup: MTU, characteristics, notification descriptor ─────────── */

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        fail(ECOFLOW_ERR_GATT, (uint8_t)error->status, "could not enable notifications");
        return 0;
    }
    /* Start the key exchange: send our public key in a plain command frame. */
    uint8_t body[42] = {0x01, 0x00};
    if (!uECC_make_key(s.pub, s.priv, uECC_secp160r1())) {
        fail(ECOFLOW_ERR_KEY, 2, "could not generate a key pair");
        return 0;
    }
    memcpy(body + 2, s.pub, sizeof s.pub);
    if (send_command(body, sizeof body)) {
        s.status.step = STEP_PUBKEY;
    }
    return 0;
}

static int on_descriptor(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0) {
        if (ble_uuid_cmp(&dsc->uuid.u, &UUID_CCCD.u) == 0) {
            s.cccd = dsc->handle;
        }
        return 0;
    }
    static const uint8_t enable[2] = {0x01, 0x00};
    if (error->status != BLE_HS_EDONE || s.cccd == 0 ||
        ble_gattc_write_flat(conn, s.cccd, enable, sizeof enable, on_subscribed, NULL) != 0) {
        fail(ECOFLOW_ERR_GATT, (uint8_t)error->status, "notification descriptor not found");
    }
    return 0;
}

static int on_characteristic(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                             void *arg)
{
    if (error->status == 0) {
        for (int i = 0; i < 2; i++) {
            /* The characteristic after the notify one bounds its descriptors. */
            if (s.pair[i].notify && !s.pair[i].notify_end && chr->val_handle > s.pair[i].notify) {
                s.pair[i].notify_end = (uint16_t)(chr->def_handle - 1);
            }
        }
        const ble_uuid_t *u = &chr->uuid.u;
        int i = (ble_uuid_cmp(u, &UUID_WRITE_16.u) == 0 || ble_uuid_cmp(u, &UUID_NOTIFY_16.u) == 0)     ? 0
                : (ble_uuid_cmp(u, &UUID_WRITE_NUS.u) == 0 || ble_uuid_cmp(u, &UUID_NOTIFY_NUS.u) == 0) ? 1
                                                                                                         : -1;
        if (i >= 0) {
            if (ble_uuid_cmp(u, &UUID_WRITE_16.u) == 0 || ble_uuid_cmp(u, &UUID_WRITE_NUS.u) == 0) {
                s.pair[i].write = chr->val_handle;
                s.pair[i].write_props = chr->properties;
            } else {
                s.pair[i].notify = chr->val_handle;
            }
        }
        return 0;
    }
    int use = (s.pair[0].write && s.pair[0].notify) ? 0 : (s.pair[1].write && s.pair[1].notify) ? 1 : -1;
    if (error->status != BLE_HS_EDONE || use < 0) {
        fail(ECOFLOW_ERR_GATT, (uint8_t)error->status, "serial characteristics not found");
        return 0;
    }
    s.gatt = s.pair[use];
    if (s.gatt.notify_end == 0) {
        s.gatt.notify_end = 0xFFFF;
    }
    if (ble_gattc_disc_all_dscs(conn, s.gatt.notify, s.gatt.notify_end, on_descriptor, NULL) != 0) {
        fail(ECOFLOW_ERR_GATT, 0, "descriptor discovery failed");
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    /* A refused exchange leaves the default MTU; discovery still works. */
    ESP_LOGI(TAG, "MTU %u (status %d)", mtu, error->status);
    if (ble_gattc_disc_all_chrs(conn, 1, 0xFFFF, on_characteristic, NULL) != 0) {
        fail(ECOFLOW_ERR_GATT, 0, "characteristic discovery failed");
    }
    return 0;
}

/* ── GAP events and the one-second tick ────────────────────────────────── */

static void on_advertisement(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields f;
    s.status.adverts++;
    if (ble_hs_adv_parse_fields(&f, disc->data, disc->length_data) != 0) {
        return;
    }
    /* Manufacturer data: company ID (2), protocol version (1), serial (16),
     * status, product type, ..., capability flags at [24]. */
    if (f.mfg_data_len < 3 + SERIAL_LEN || (f.mfg_data[0] | (f.mfg_data[1] << 8)) != COMPANY_ID) {
        return;
    }
    s.status.units_seen++;
    if (memcmp(f.mfg_data + 3, s.serial, SERIAL_LEN) != 0) {
        return;
    }
    s.status.rssi = disc->rssi;
    s.status.encrypt_type = f.mfg_data_len > 24 ? (f.mfg_data[24] >> 3) & 0x07 : 7;
    if (s.status.encrypt_type != 7) {
        if (s.status.last_error != ECOFLOW_ERR_ENCRYPTION) {
            ESP_LOGE(TAG, "unit uses encryption type %u; only type 7 is implemented", s.status.encrypt_type);
        }
        s.status.last_error = ECOFLOW_ERR_ENCRYPTION;
        s.status.last_error_detail = s.status.encrypt_type;
        return;
    }
    ESP_LOGI(TAG, "unit found, RSSI %d dBm; connecting", disc->rssi);
    ble_gap_disc_cancel();
    if (ble_gap_connect(s.own_addr_type, &disc->addr, 30000, NULL, gap_event, NULL) == 0) {
        s.status.state = ECOFLOW_CONNECTING;
    } else {
        link_down();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (s.status.state == ECOFLOW_SCANNING) {
            on_advertisement(&event->disc);
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s.status.state == ECOFLOW_SCANNING) {
            link_down(); /* the scan ended on its own: start another */
        }
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            link_down();
            break;
        }
        s.conn = event->connect.conn_handle;
        s.status.state = ECOFLOW_HANDSHAKE;
        s.status.step = STEP_DISCOVER;
        s.deadline_ms = now_ms() + HANDSHAKE_MS;
        if (ble_gattc_exchange_mtu(s.conn, on_mtu, NULL) != 0) {
            fail(ECOFLOW_ERR_GATT, 0, "MTU exchange failed");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "link down, reason %d", event->disconnect.reason);
        s.status.drops++;
        link_down();
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == s.conn && event->notify_rx.attr_handle == s.gatt.notify) {
            on_notify(event->notify_rx.om);
        }
        break;
    default:
        break;
    }
    return 0;
}

static void on_tick(struct ble_npl_event *ev)
{
    int64_t now = now_ms();
    if (s.status.state == ECOFLOW_IDLE && now >= s.retry_at_ms) {
        start_scan();
    } else if (s.deadline_ms && now > s.deadline_ms) {
        fail(ECOFLOW_ERR_TIMEOUT, s.status.step, "timed out waiting for the unit");
    } else if (s.status.state == ECOFLOW_ONLINE) {
        int8_t rssi;
        if (ble_gap_conn_rssi(s.conn, &rssi) == 0) {
            s.status.rssi = rssi;
        }
    }
    ble_npl_callout_reset(&s.tick, ble_npl_time_ms_to_ticks32(1000));
}

static void on_sync(void)
{
    /* Also reached after a controller reset, when any old link is gone. */
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s.own_addr_type);
    link_down();
    s.backoff_ms = BACKOFF_MIN_MS;
    s.retry_at_ms = 0;
    ble_npl_callout_reset(&s.tick, ble_npl_time_ms_to_ticks32(1000));
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ecoflow_ble_start(const char *serial, const char *user_id, ecoflow_inverter_cb on_inverter,
                       ecoflow_soc_cb on_soc)
{
    s.serial = serial;
    s.user_id = user_id;
    s.on_inverter = on_inverter;
    s.on_soc = on_soc;
    s.conn = BLE_HS_CONN_HANDLE_NONE;
    if (strlen(serial) != SERIAL_LEN) {
        ESP_LOGE(TAG, "the serial number in secrets.h must be %d characters; Bluetooth not started", SERIAL_LEN);
        return;
    }
    uECC_set_rng(rng);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_npl_callout_init(&s.tick, nimble_port_get_dflt_eventq(), on_tick, NULL);
    nimble_port_freertos_init(host_task);
}

void ecoflow_ble_status(ecoflow_ble_status_t *out)
{
    *out = s.status; /* unlocked read of a few counters: diagnostics only */
}

#if CONFIG_CANARY_TEST_COMMANDS
size_t ecoflow_ble_raw(char *out, size_t cap)
{
    size_t n = (size_t)snprintf(out, cap, "!inv=");
    for (int i = 0; i < s.raw_inv_len && n + 3 < cap; i++) {
        n += (size_t)snprintf(out + n, cap - n, "%02x", s.raw_inv[i]);
    }
    n += (size_t)snprintf(out + n, cap - n, " ems=");
    for (int i = 0; i < s.raw_ems_len && n + 3 < cap; i++) {
        n += (size_t)snprintf(out + n, cap - n, "%02x", s.raw_ems[i]);
    }
    n += (size_t)snprintf(out + n, cap - n, "\r");
    return n < cap ? n : 0;
}
#endif
