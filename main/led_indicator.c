/* Status-LED hardware glue (receiver, ESP32-C6).
 *
 * See led_indicator.h. This file is compiled only for the ESP32-C6 receiver,
 * the only board with the addressable WS2812 LED. Everywhere else (the sender,
 * and the classic-ESP32 receiver whose default GPIO 8 is a SPI-flash pin) the
 * functions are inert stubs, so main.c can call them unconditionally and the
 * default led_gpio=8 can never be driven on a board where it would clash with
 * the flash.
 *
 * Power discipline:
 *  - the strip is written only when the effective (colour, on/off) pair changes,
 *  - the colour is scaled by the persisted brightness (default 16/255),
 *  - the pulse is short and infrequent (a low-duty "heartbeat"),
 *  - led off / led-gpio 0 releases the RMT driver (no idle current). */
#include "led_indicator.h"

#if !CONFIG_ESPNOW_ROLE_SENDER && defined(CONFIG_IDF_TARGET_ESP32C6)

#include <stddef.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

static const char *TAG = "led";

/* Tick fast enough to place the shortest pulse (100 ms) with a little margin,
 * but slowly enough to be negligible: ~5 % CPU-free overhead, and the strip is
 * written only on a change. */
#define LED_TICK_US 50000 /* 50 ms */

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    bool on;
} led_effective_t;

static led_strip_handle_t s_strip;
static esp_timer_handle_t s_timer;
static bool s_active; /* driver created */
static uint8_t s_gpio; /* GPIO the driver is bound to */

static led_status_t s_status = LED_STATUS_BOOT;
static led_status_t s_override = LED_STATUS_COUNT; /* COUNT == automatic */
static uint32_t s_phase_start_ms;
static uint8_t s_brightness = 16;

/* Last value actually written, so an unchanged frame is never re-sent. */
static led_effective_t s_last;
static bool s_last_valid;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void write_effective(const led_effective_t *want)
{
    if (!s_active || s_strip == NULL) {
        return;
    }
    uint8_t r = want->on ? want->r : 0u;
    uint8_t g = want->on ? want->g : 0u;
    uint8_t b = want->on ? want->b : 0u;
    if (led_strip_set_pixel(s_strip, 0, r, g, b) != ESP_OK) {
        return;
    }
    /* A single refresh latches the frame; no periodic refresh follows. */
    (void)led_strip_refresh(s_strip);
}

static void tick(void *arg)
{
    (void)arg;
    if (!s_active) {
        return;
    }
    led_status_t status = s_override < LED_STATUS_COUNT ? s_override : s_status;
    led_status_output_t out;
    led_status_output(status, now_ms() - s_phase_start_ms, &out);
    led_effective_t want = {
        .r = led_status_scale(out.r, s_brightness),
        .g = led_status_scale(out.g, s_brightness),
        .b = led_status_scale(out.b, s_brightness),
        .on = out.on,
    };
    if (s_last_valid && want.r == s_last.r && want.g == s_last.g &&
        want.b == s_last.b && want.on == s_last.on) {
        return; /* no change: do not touch the strip */
    }
    write_effective(&want);
    s_last = want;
    s_last_valid = true;
}

static void release_driver(void)
{
    if (s_timer != NULL) {
        (void)esp_timer_stop(s_timer);
        (void)esp_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_strip != NULL) {
        /* Drive the pixel dark before freeing so a release never leaves the LED
         * latched on. */
        (void)led_strip_clear(s_strip);
        (void)led_strip_del(s_strip);
        s_strip = NULL;
    }
    s_active = false;
    s_last_valid = false;
}

static bool create_driver(uint8_t gpio, uint8_t brightness)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = (int)gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        /* 10 MHz resolution and the default RMT block keep the driver small;
         * DMA is unnecessary for a single pixel. */
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK || s_strip == NULL) {
        ESP_LOGW(TAG, "LED driver create failed on GPIO %u: %s", (unsigned)gpio,
                 esp_err_to_name(err));
        s_strip = NULL;
        return false;
    }
    (void)led_strip_clear(s_strip); /* start dark, then let the tick light it */
    s_brightness = brightness;
    s_gpio = gpio;
    s_active = true;

    const esp_timer_create_args_t timer_args = {
        .callback = tick,
        .name = "led_tick",
    };
    if (esp_timer_create(&timer_args, &s_timer) != ESP_OK ||
        esp_timer_start_periodic(s_timer, LED_TICK_US) != ESP_OK) {
        ESP_LOGW(TAG, "LED timer start failed; releasing the driver");
        release_driver();
        return false;
    }
    ESP_LOGI(TAG, "Status LED on GPIO %u (brightness %u)", (unsigned)gpio,
             (unsigned)brightness);
    return true;
}

void led_indicator_apply_config(bool enabled, uint8_t gpio, uint8_t brightness)
{
    bool want = enabled && gpio != 0u;
    if (!want) {
        if (s_active) {
            ESP_LOGI(TAG, "Status LED disabled; releasing the RMT driver");
        }
        release_driver();
        return;
    }
    /* Rebuild only when the GPIO changed (the driver binds the pin at create
     * time). A brightness-only change is applied by the next tick. */
    if (!s_active || s_gpio != gpio) {
        release_driver();
        (void)create_driver(gpio, brightness);
    } else {
        s_brightness = brightness;
    }
}

void led_indicator_set_status(led_status_t status)
{
    if (status == s_status) {
        return;
    }
    s_status = status;
    s_phase_start_ms = now_ms();
}

void led_indicator_set_override(led_status_t override_status)
{
    if (override_status == s_override) {
        return;
    }
    s_override = override_status;
    s_phase_start_ms = now_ms();
}

led_status_t led_indicator_status(void)
{
    return s_status;
}

bool led_indicator_is_active(void)
{
    return s_active;
}

#else /* sender, or a receiver on a board without the WS2812 LED: inert stubs */

void led_indicator_apply_config(bool enabled, uint8_t gpio, uint8_t brightness)
{
    (void)enabled;
    (void)gpio;
    (void)brightness;
}

void led_indicator_set_status(led_status_t status)
{
    (void)status;
}

void led_indicator_set_override(led_status_t override_status)
{
    (void)override_status;
}

led_status_t led_indicator_status(void)
{
    return LED_STATUS_BOOT;
}

bool led_indicator_is_active(void)
{
    return false;
}

#endif
