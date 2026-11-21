#include "sense.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "sense";

#define CH_VSYS ADC_CHANNEL_3  // IO3
#define CH_VBUS ADC_CHANNEL_4  // IO4
#define PIN_VBUS_DIGITAL GPIO_NUM_4
#define SAMPLES 16

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali[2];

static adc_cali_handle_t make_cali(adc_channel_t ch)
{
    adc_cali_handle_t h = NULL;
    adc_cali_curve_fitting_config_t cc = {
        .unit_id = ADC_UNIT_1,
        .chan = ch,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cc, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration for ch%d, using raw scaling", ch);
        h = NULL;
    }
    return h;
}

static void unit_init(void)
{
    adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, CH_VSYS, &c));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, CH_VBUS, &c));
    // The channel setup turns the pad's digital input off; VBUS presence is read digitally too
    // (0.6 x VBUS: high above about 4.1 V), so it needs no ADC conversion while off.
    gpio_input_enable(PIN_VBUS_DIGITAL);
}

void sense_init(void)
{
    unit_init();
    s_cali[0] = make_cali(CH_VSYS);
    s_cali[1] = make_cali(CH_VBUS);
}

static int s_vsys_raw, s_vsys_pin;

static int read_pin_mv(adc_channel_t ch, adc_cali_handle_t cali, int *raw_out)
{
    int sum = 0, n = 0;
    for (int i = 0; i < SAMPLES; i++) {
        int raw;
        esp_err_t err = adc_oneshot_read(s_adc, ch, &raw);
        if (err == ESP_OK) {
            sum += raw;
            n++;
        } else {
            ESP_LOGW(TAG, "ch%d read: %s", ch, esp_err_to_name(err));
        }
    }
    if (!n) return -1;
    int raw = sum / n;
    if (raw_out) *raw_out = raw;
    int mv;
    if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) return mv;
    return raw * 3300 / 4095;
}

// A pin can't sit above the chip's own supply (3.3 V, plus a diode drop at most), so a reading
// above 3.6 V is the ADC stuck, not a voltage. That happens once light sleep has run: both
// channels read 4095 (8.1 V VSYS, 6.8 V VBUS) until the unit is rebuilt. While off only VBUS is
// read, and a stuck VBUS looked like USB, which holds the no-light-sleep lock.
#define PIN_MAX_MV 3600
static int s_rebuilds;

int sense_adc_rebuilds(void) { return s_rebuilds; }

static int read_checked(adc_channel_t ch, adc_cali_handle_t cali, int *raw_out)
{
    int raw = 0;
    int mv = read_pin_mv(ch, cali, &raw);
    if (mv > PIN_MAX_MV) {
        adc_oneshot_del_unit(s_adc);
        unit_init();
        int before = raw;
        mv = read_pin_mv(ch, cali, &raw);
        if (++s_rebuilds <= 5 || s_rebuilds % 100 == 0)
            ESP_LOGW(TAG, "ch%d ADC stuck at %d, rebuilt the unit (%d so far): now %d", ch, before, s_rebuilds, raw);
    }
    if (raw_out) *raw_out = raw;
    return mv;
}

int sense_vsys_mv(void)
{
    s_vsys_pin = read_checked(CH_VSYS, s_cali[0], &s_vsys_raw);
    return s_vsys_pin * 2;
}

void sense_vsys_debug(int *raw, int *pin_mv)
{
    *raw = s_vsys_raw;
    *pin_mv = s_vsys_pin;
}
int sense_vbus_mv(void) { return read_checked(CH_VBUS, s_cali[1], NULL) * 10 / 6; }
bool sense_vbus_present(void) { return gpio_get_level(PIN_VBUS_DIGITAL) != 0; }
