#include "config.h"
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    c->version = CONFIG_VERSION;
    c->mode = MODE_TWO_TIMERS;
    c->count_up = 0;
    c->show_numbers = 1;
    c->initial_owner = 0;
    c->default_capture = 0;  // random
    // Half volume and LED brightness: full volume browns out on USB-only power
    // (the 400 mA charger feeds VSYS). Turn them up when running on a cell.
    c->volume = 128;
    c->led_level = 128;
    c->hold_ms = 3000;
    c->duration_ms[0] = c->duration_ms[1] = 10 * 60 * 1000;
    static const uint8_t COL[2][3] = {{255, 0, 128}, {255, 128, 0}};  // purple, yellow. (blue die is washed out)
    memcpy(c->colour, COL, sizeof COL);
}

void config_sanitise(config_t *c)
{
    c->version = CONFIG_VERSION;
    if (c->mode > MODE_TWO_TIMERS) c->mode = MODE_TWO_TIMERS;
    c->reserved2 = 0;
    c->count_up = c->count_up ? 1 : 0;
    c->show_numbers = c->show_numbers ? 1 : 0;
    if (c->initial_owner > 2) c->initial_owner = 0;
    if (c->default_capture > 2) c->default_capture = 0;
    if (c->hold_ms < 500) c->hold_ms = 500;
    if (c->hold_ms > 60000) c->hold_ms = 60000;
    for (int t = 0; t < 2; t++) {
        if (c->duration_ms[t] > 24u * 3600 * 1000) c->duration_ms[t] = 24u * 3600 * 1000;
        if (!c->count_up && c->duration_ms[t] < 1000) c->duration_ms[t] = 1000;  // 0 only means "no limit" counting up
    }
    c->name[sizeof c->name - 1] = 0;
}

void config_load(config_t *c)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    config_defaults(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_handle_t h;
    if (nvs_open("button", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return;
    }
    config_t tmp;
    size_t len = sizeof tmp;
    if (nvs_get_blob(h, "cfg", &tmp, &len) == ESP_OK && len == sizeof tmp && tmp.version == CONFIG_VERSION) {
        config_sanitise(&tmp);
        *c = tmp;
        ESP_LOGI(TAG, "loaded saved settings");
    } else {
        ESP_LOGI(TAG, "saved settings missing or old, using defaults");
    }
    nvs_close(h);
}

void config_save(const config_t *c)
{
    nvs_handle_t h;
    if (nvs_open("button", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "cfg", c, sizeof *c) != ESP_OK || nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "save failed");
    nvs_close(h);
}

uint8_t config_load_power(void)
{
    uint8_t p = 0;
    nvs_handle_t h;
    if (nvs_open("button", NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u8(h, "pwr", &p) != ESP_OK || p > 2) p = 0;
    nvs_close(h);
    return p;
}

void config_save_power(uint8_t p)
{
    nvs_handle_t h;
    if (nvs_open("button", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "pwr", p) != ESP_OK || nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "power state save failed");
    nvs_close(h);
}

bool config_firmware_changed(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    uint8_t old[8] = {0};
    size_t len = sizeof old;
    nvs_handle_t h;
    if (nvs_open("button", NVS_READWRITE, &h) != ESP_OK) return false;
    bool changed = nvs_get_blob(h, "fw", old, &len) != ESP_OK || len != sizeof old ||
                   memcmp(old, d->app_elf_sha256, sizeof old) != 0;
    if (changed && (nvs_set_blob(h, "fw", d->app_elf_sha256, sizeof old) != ESP_OK || nvs_commit(h) != ESP_OK))
        ESP_LOGW(TAG, "firmware hash save failed");
    nvs_close(h);
    return changed;
}
