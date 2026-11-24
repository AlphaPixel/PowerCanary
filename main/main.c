/* SPDX-License-Identifier: MIT
 *
 * PowerCanary: reads a power station over Bluetooth LE and answers a host on
 * the USB serial port as a Megatec/Q1 UPS.
 *
 * The USB serial port carries the Megatec protocol and nothing else. Logs go
 * to UART0 (see sdkconfig.defaults), so they can never corrupt a reply.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "esp_rom_sys.h"
#include "soc/lp_aon_struct.h"
#include "soc/usb_serial_jtag_struct.h"

#include "ecoflow_ble.h"
#include "megatec.h"
#include "power_state.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "main/secrets.h is missing: run tools/ecoflow_secrets.py or copy main/secrets.h.example"
#endif

static const char *TAG = "canary";

static const power_cfg_t CFG = {
    .ac_present_min_mv = CONFIG_CANARY_AC_PRESENT_MIN_V * 1000u,
    .debounce_ms = CONFIG_CANARY_DEBOUNCE_S * 1000u,
    .stale_ms = CONFIG_CANARY_STALE_S * 1000u,
    .low_batt_pct = CONFIG_CANARY_LOW_BATTERY_PCT,
};
static const megatec_rated_t RATED = {
    .volts = CONFIG_CANARY_RATED_VOLTS,
    .watts = CONFIG_CANARY_RATED_WATTS,
    .hz = CONFIG_CANARY_RATED_HZ,
};

static power_state_t s_state;
static SemaphoreHandle_t s_lock; /* guards s_state between the Bluetooth and serial tasks */
static bool s_simulating;        /* test builds only: SIM samples replace Bluetooth ones */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void on_inverter(const ef_inverter_t *inv)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_simulating) {
        power_state_set_ac(&s_state, &CFG, now_ms(), inv->ac_in_mv, inv->ac_out_mv, inv->out_watts, inv->ac_in_hz);
    }
    xSemaphoreGive(s_lock);
}

static void on_soc(uint8_t soc)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_simulating) {
        power_state_set_soc(&s_state, now_ms(), soc);
    }
    xSemaphoreGive(s_lock);
}

/* DBG: one human-readable status line. It starts with '!' so it can never be
 * taken for a Megatec reply, and it carries no secrets. */
static size_t debug_line(char *out, size_t cap)
{
    ecoflow_ble_status_t b;
    power_state_t st;
    power_view_t v = {0};
    int64_t now = now_ms();

    ecoflow_ble_status(&b);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_state;
    bool fresh = power_state_view(&s_state, &CFG, now, &v);
    xSemaphoreGive(s_lock);

    int n = snprintf(out, cap,
                     "!fw=" CANARY_VERSION " up=%lld ble=%u/%u adv=%lu seen=%lu enc=%u rssi=%d sessions=%lu"
                     " drops=%lu bad=%lu inv=%lu ems=%lu err=%u/%u"
                     " ac_in_mv=%lu ac_out_mv=%lu w=%u hz=%u soc=%u ac_age_ms=%lld soc_age_ms=%lld"
                     " fresh=%d ob=%d lb=%d sim=%d rst=%d usj=%lx\r",
                     (long long)(now / 1000), b.state, b.step, (unsigned long)b.adverts,
                     (unsigned long)b.units_seen, b.encrypt_type, b.rssi, (unsigned long)b.sessions,
                     (unsigned long)b.drops, (unsigned long)b.bad_packets, (unsigned long)b.inverter_packets,
                     (unsigned long)b.ems_packets, b.last_error, b.last_error_detail,
                     (unsigned long)st.ac_in_mv, (unsigned long)st.ac_out_mv, st.out_watts, st.ac_in_hz, st.soc,
                     (long long)(st.ac_at_ms < 0 ? -1 : now - st.ac_at_ms),
                     (long long)(st.soc_at_ms < 0 ? -1 : now - st.soc_at_ms), fresh, v.on_battery, v.low_battery,
                     s_simulating, (int)esp_reset_reason(), (unsigned long)USB_SERIAL_JTAG.chip_rst.val);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

#if CONFIG_CANARY_TEST_COMMANDS
/* SIM <ac_in_mv> <soc> injects one sample and stops Bluetooth samples from
 * being used; SIM OFF returns to the real unit. Repeat it to keep the data
 * fresh, or stop to watch the stale rule take effect. */
static size_t simulate(const char *args, char *out, size_t cap)
{
    char *end;
    unsigned long mv = strtoul(args, &end, 10);
    unsigned long soc = strtoul(end, NULL, 10);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (end == args) {
        s_simulating = false;
        power_state_init(&s_state);
    } else {
        s_simulating = true;
        power_state_set_ac(&s_state, &CFG, now_ms(), mv, CONFIG_CANARY_RATED_VOLTS * 1000u, 0,
                           mv ? CONFIG_CANARY_RATED_HZ : 0);
        power_state_set_soc(&s_state, now_ms(), (uint8_t)(soc > 100 ? 100 : soc));
    }
    xSemaphoreGive(s_lock);
    int n = snprintf(out, cap, "!sim=%d\r", s_simulating);
    return n > 0 ? (size_t)n : 0;
}
#endif

static void handle_line(const char *line)
{
    char out[512];
    size_t len;

    if (strcmp(line, "DBG") == 0) {
        len = debug_line(out, sizeof out);
#if CONFIG_CANARY_TEST_COMMANDS
    } else if (strncmp(line, "SIM", 3) == 0) {
        len = simulate(line + 3, out, sizeof out);
    } else if (strcmp(line, "RAW") == 0) {
        len = ecoflow_ble_raw(out, sizeof out);
    } else if (strcmp(line, "BOOT") == 0) {
        LP_AON.sys_cfg.force_download_boot = 1; /* next reset enters the USB download bootloader */
        esp_restart();
#endif
    } else {
        power_view_t v;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool fresh = power_state_view(&s_state, &CFG, now_ms(), &v);
        xSemaphoreGive(s_lock);
        len = megatec_reply(line, fresh ? &v : NULL, &RATED, out, sizeof out);
    }
    if (len) {
        /* If nobody is reading, drop the reply rather than block. */
        usb_serial_jtag_write_bytes(out, len, pdMS_TO_TICKS(200));
    }
}

static void serial_task(void *arg)
{
    char line[24];
    size_t n = 0;
    bool overflow = false;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        uint8_t buf[32];
        int got = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(500));
        esp_task_wdt_reset();
        for (int i = 0; i < got; i++) {
            if (buf[i] == '\r' || buf[i] == '\n') {
                line[n] = '\0';
                if (n > 0 && !overflow) {
                    handle_line(line);
                }
                n = 0;
                overflow = false;
            } else if (n < sizeof line - 1) {
                line[n++] = (char)buf[i];
            } else {
                overflow = true; /* longer than any command: ignore the whole line */
            }
        }
    }
}

void app_main(void)
{
    /* The ROM mirrors its console to the USB port. Stop that, so the port
     * carries Megatec replies only, and keep our own logs on UART0. */
    esp_rom_install_channel_putc(2, NULL);
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_err_t err = nvs_flash_init(); /* the radio keeps its calibration here */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_lock = xSemaphoreCreateMutex();
    power_state_init(&s_state);

    usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 1024, .rx_buffer_size = 256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
#if CONFIG_CANARY_USB_RESET_DISABLE
    /* A host opening or closing the serial port toggles DTR and RTS, which
     * the USB serial controller would otherwise turn into a chip reset and a
     * dropped Bluetooth session. Consequence: flashing needs the BOOT button.
     * Two details matter. The write must come after the driver install,
     * because until then the controller's register clock is gated off. And
     * the bit does nothing until config_update copies it into the
     * controller's own clock domain. */
    USB_SERIAL_JTAG.chip_rst.usb_uart_chip_rst_dis = 1;
    USB_SERIAL_JTAG.config_update.config_update = 1;
#endif

    ESP_LOGI(TAG, "PowerCanary " CANARY_VERSION ": debounce %d s, low battery %d%%, stale %d s",
             CONFIG_CANARY_DEBOUNCE_S, CONFIG_CANARY_LOW_BATTERY_PCT, CONFIG_CANARY_STALE_S);

    xTaskCreate(serial_task, "megatec", 4096, NULL, 5, NULL);
    ecoflow_ble_start(ECOFLOW_SERIAL, ECOFLOW_USER_ID, on_inverter, on_soc);
}
