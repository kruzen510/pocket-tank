/* battery_port_es3c28p.c — the Hosyond / LCDWIKI ESP32-S3 2.8in (ES3C28P) power
 * side, behind the same battery_port.h the AXP2101 board uses. There is no PMIC
 * and no power key here (a TP4054 charges the cell; RESET and BOOT are the only
 * buttons), so:
 *   - the gauge is VBAT through a divider on GPIO9 (ADC1), turned into a
 *     percentage with a resting Li-ion curve: an estimate, high while charging,
 *     a little low under load. The divider ratio (board_pins.h BAT_DIV_*) is an
 *     assumption until someone checks it against a meter on a real cell;
 *   - "the cable" is the ESP32-S3's own USB: the USB-Serial-JTAG sees the host's
 *     start-of-frame packets. A wall charger with no data lines looks like "on
 *     battery" (the pill then shows the voltage-based level);
 *   - init returns false ("no PMIC"): power-off is not a thing, main.c sleeps
 *     the tank the no-PMIC way - BOOT is the sleep key, deep sleep after the
 *     grace, BOOT wakes it. */
#include "battery_port.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "battery";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_ok;
static int64_t s_last_us = -1;
static int s_mv;                                /* filtered VBAT */
static float s_frac; static int s_state = BAT_ON_BATTERY; static bool s_valid;

/* resting Li-ion open-circuit curve, mV at 0,10..100 % */
static const int OCV[11] = { 3300, 3600, 3690, 3740, 3770, 3800, 3850, 3920, 4000, 4080, 4180 };
static float mv_to_frac(int mv) {
    if (mv <= OCV[0]) return 0;
    if (mv >= OCV[10]) return 1;
    for (int i = 1; i <= 10; i++)
        if (mv < OCV[i]) return ((i - 1) + (float)(mv - OCV[i - 1]) / (OCV[i] - OCV[i - 1])) / 10.0f;
    return 1;
}

static int read_pin_mv(void) {
    if (!s_adc) return 0;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3100 / 4095;          /* uncalibrated: 12 dB range ~3.1 V */
        n++;
    }
    return n ? sum / n : 0;
}
static int read_mv_raw(void) { return read_pin_mv() * BAT_DIV_NUM / BAT_DIV_DEN; }

bool battery_port_init(i2c_master_bus_handle_t bus) {
    (void)bus;
    adc_unit_t unit;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_io_to_channel(PIN_BAT_ADC, &unit, &s_chan) != ESP_OK || unit != ADC_UNIT_1 ||
        adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) { s_adc = NULL; ESP_LOGW(TAG, "ADC unavailable: no gauge"); return false; }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, s_chan, &ccfg);
    adc_cali_curve_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .chan = s_chan,
                                           .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cc, &s_cali) != ESP_OK) s_cali = NULL;
    int pin_mv = read_pin_mv();
    s_mv = pin_mv * BAT_DIV_NUM / BAT_DIV_DEN;
    s_ok = true;
    ESP_LOGI(TAG, "ES3C28P power: GPIO%d reads %d mV -> VBAT %d mV (x%d/%d assumed)%s; no PMIC, BOOT is the sleep key",
             PIN_BAT_ADC, pin_mv, s_mv, BAT_DIV_NUM, BAT_DIV_DEN, s_cali ? ", calibrated" : "");
    return false;                               /* no PMIC: main.c sleeps the no-PMIC way */
}

static void refresh(void) {
    int64_t now = esp_timer_get_time();
    if (s_last_us >= 0 && now - s_last_us < 1000000) return;
    s_last_us = now;
    int mv = read_mv_raw();
    s_mv = s_mv ? (s_mv * 3 + mv) / 4 : mv;     /* light smoothing: the ADC wobbles tens of mV */
    bool usb = usb_serial_jtag_is_connected();
    s_valid = s_ok && s_mv > 2800 && s_mv < 4500;   /* outside that: no cell on the connector */
    s_frac = mv_to_frac(s_mv);
    s_state = !usb ? BAT_ON_BATTERY : s_mv >= 4150 ? BAT_FULL : BAT_CHARGING;
}

bool battery_port_read(float *frac, bool *charging) {
    refresh();
    if (!s_valid) return false;
    if (frac) *frac = s_frac;
    if (charging) *charging = s_state == BAT_CHARGING;
    return true;
}
int battery_port_state(void) { refresh(); return s_state; }
int battery_port_vbat_mv(void) { refresh(); return s_valid ? s_mv : 0; }

bool battery_port_poweroff(void) { return false; }        /* no PMIC, no switch: main.c deep-sleeps instead */

/* no PWR key on this board */
void battery_port_key_init(void) {}
int  battery_port_key_poll(void) { return 0; }
void battery_port_key_trace(int seconds) { (void)seconds; ESP_LOGW(TAG, "no PWR key on this board (BOOT is the sleep key)"); }

void battery_port_dump(void) {
    refresh();
    ESP_LOGI(TAG, "ES3C28P: GPIO%d %d mV -> VBAT %d mV (%s), ~%.0f%%, USB %s", PIN_BAT_ADC, read_pin_mv(), s_mv,
             s_valid ? "cell present" : "no cell?", s_frac * 100, usb_serial_jtag_is_connected() ? "connected" : "not seen");
}
/* silent: audio_port_es8311.c asks for its codec rail on every cue; here the codec's supply is always on */
bool battery_port_set_rail(const char *name, bool on) { (void)name; (void)on; return false; }
void battery_port_trim_rails(void) {}
