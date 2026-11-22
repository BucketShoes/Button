#include "config.h"
#include <math.h>
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
    // Team 1 purple, team 2 yellow. The numbers don't look like that on a screen (they look pink and
    // orange), because they're tuned for these LEDs, whose green is far brighter than the red and
    // whose blue is cyan-ish: 255/255/0 shows plainly green and 0/255/255 nearly white. They're screen
    // values (the LEDs decode them, see team_colour in main.c), the same LED mix as the 255/0/128 and
    // 255/127/0 that were tuned by eye before that.
    static const uint8_t COL[2][3] = {{255, 0, 186}, {255, 186, 0}};  // purple, yellow (on the LEDs)
    memcpy(c->colour, COL, sizeof COL);
    c->sound_set = SOUNDS_MEDIEVAL;
    c->sparse_min = 11;
    c->dim_min = 31;
    c->auto_off_min = 61;
    c->switch_off_min = 121;
}

void config_sanitise(config_t *c)
{
    c->version = CONFIG_VERSION;
    if (c->mode > MODE_TWO_TIMERS) c->mode = MODE_TWO_TIMERS;
    if (c->sound_set >= SOUNDS_COUNT) c->sound_set = SOUNDS_SCIFI;
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
    esp_err_t got = nvs_get_blob(h, "cfg", &tmp, &len);
    if (got == ESP_OK && len == sizeof tmp && tmp.version == CONFIG_VERSION) {
        config_sanitise(&tmp);
        *c = tmp;
        ESP_LOGI(TAG, "loaded saved settings");
    } else if (got == ESP_OK && len == CONFIG_V2_LEN && tmp.version == 2) {
        memcpy(c, &tmp, CONFIG_V2_LEN);  // the idle times keep their defaults
        // Version 2 colours were sent to the LEDs as they were; now they are decoded as screen
        // colours first. Encoded here, so the LEDs look the same as before.
        for (int t = 0; t < 2; t++)
            for (int i = 0; i < 3; i++) c->colour[t][i] = (uint8_t)(255.0f * powf(c->colour[t][i] / 255.0f, 1 / 2.2f) + 0.5f);
        config_sanitise(c);
        ESP_LOGI(TAG, "loaded saved settings (version 2, idle times defaulted)");
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

bool config_blob_load(const char *key, void *p, size_t n)
{
    nvs_handle_t h;
    if (nvs_open("button", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    bool ok = nvs_get_blob(h, key, p, &len) == ESP_OK && len == n;
    nvs_close(h);
    return ok;
}

void config_blob_save(const char *key, const void *p, size_t n)
{
    nvs_handle_t h;
    if (nvs_open("button", NVS_READWRITE, &h) != ESP_OK) return;
    // (Reading first: an unchanged value isn't written again, which saves flash wear on repeated sleeps.)
    uint8_t old[64];
    size_t len = sizeof old;
    if (n <= sizeof old && nvs_get_blob(h, key, old, &len) == ESP_OK && len == n && memcmp(old, p, n) == 0) {
        nvs_close(h);
        return;
    }
    if (nvs_set_blob(h, key, p, n) != ESP_OK || nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "%s save failed", key);
    nvs_close(h);
}

void config_blob_erase(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("button", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, key) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}
