#include "sense.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "sense";

#define CH_VSYS ADC_CHANNEL_3  // IO3
#define CH_VBUS ADC_CHANNEL_4  // IO4
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

// VSYS tops out at 4.2 V, 2.1 V at the pin, so a raw reading near full scale is the ADC
// stuck, not a voltage. Seen once light sleep had run (on battery): both channels read 4095
// from then on. Rebuild the ADC unit and read again; the log says whether that cleared it.
#define RAW_STUCK 4000
static int s_reinits;

int sense_vsys_mv(void)
{
    s_vsys_pin = read_pin_mv(CH_VSYS, s_cali[0], &s_vsys_raw);
    if (s_vsys_raw >= RAW_STUCK) {
        int before = s_vsys_raw;
        adc_oneshot_del_unit(s_adc);
        unit_init();
        s_reinits++;
        s_vsys_pin = read_pin_mv(CH_VSYS, s_cali[0], &s_vsys_raw);
        ESP_LOGW(TAG, "VSYS ADC stuck at %d, rebuilt the ADC unit (%d time(s)): now %d", before,
                 s_reinits, s_vsys_raw);
    }
    return s_vsys_pin * 2;
}

void sense_vsys_debug(int *raw, int *pin_mv)
{
    *raw = s_vsys_raw;
    *pin_mv = s_vsys_pin;
}
int sense_vbus_mv(void) { return read_pin_mv(CH_VBUS, s_cali[1], NULL) * 10 / 6; }
