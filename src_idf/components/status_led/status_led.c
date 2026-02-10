/**
 * Status LED Component Implementation
 *
 * Uses LEDC (PWM) for smooth LED animations.
 */

#include "status_led.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "status_led";

// LEDC configuration (use timer 1 / channel 1 to avoid conflict with camera XCLK on ch 0)
#define LEDC_TIMER          LEDC_TIMER_1
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_1
#define LEDC_FREQ_HZ        5000
#define LEDC_RESOLUTION     LEDC_TIMER_8_BIT
#define LEDC_MAX_DUTY       ((1 << 8) - 1)

// Timing constants (from Arduino version)
#define RING_LED_DURATION_MS        6000
#define DOUBLE_BLINK_PERIOD_MS      1000
#define WIFI_BLINK_PERIOD_MS        500     // 2 Hz
#define SIP_PULSE_PERIOD_MS         2000    // Slow pulse
#define RING_PULSE_PERIOD_MS        1400    // Breathing ring
#define RTSP_TICK_PERIOD_MS         2000
#define RTSP_TICK_ON_MS             80

// Duty cycle constants
#define DUTY_LOW            24
#define DUTY_PULSE_MAX      180
#define DUTY_PULSE_MIN      8
#define DUTY_BLINK          200
#define DUTY_RING_MAX       220
#define DUTY_RTSP_TICK      200

// State tracking
static uint32_t ring_until_ms = 0;
static uint8_t state_flags = 0;
static uint16_t last_duty = 0;

// Helper to get milliseconds
static uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Set state flag bit
static void set_state_flag(led_state_t state, bool active) {
    if (active) {
        state_flags |= (1 << state);
    } else {
        state_flags &= ~(1 << state);
    }
}

// Check state flag bit
static bool get_state_flag(led_state_t state) {
    return (state_flags & (1 << state)) != 0;
}

// Triangle wave for breathing effect
static uint8_t triangle_wave(uint32_t now, uint32_t period_ms, uint8_t min_duty, uint8_t max_duty) {
    if (period_ms == 0 || max_duty <= min_duty) {
        return max_duty;
    }
    uint32_t phase = now % period_ms;
    uint32_t half = period_ms / 2;
    uint32_t span = max_duty - min_duty;
    if (phase < half) {
        return (uint8_t)(min_duty + (span * phase) / half);
    }
    return (uint8_t)(max_duty - (span * (phase - half)) / half);
}

// Double blink pattern for AP mode
static uint8_t double_blink(uint32_t now) {
    uint32_t phase = now % DOUBLE_BLINK_PERIOD_MS;
    bool on = (phase < 80) || (phase >= 160 && phase < 240);
    return on ? DUTY_BLINK : 0;
}

// Simple blink pattern
static uint8_t blink(uint32_t now, uint32_t period_ms) {
    uint32_t phase = now % period_ms;
    return (phase < (period_ms / 2)) ? DUTY_BLINK : 0;
}

// RTSP tick pattern
static uint8_t rtsp_tick(uint32_t now) {
    uint32_t phase = now % RTSP_TICK_PERIOD_MS;
    return (phase < RTSP_TICK_ON_MS) ? DUTY_RTSP_TICK : 0;
}

// Set LED duty cycle
static void set_duty(uint16_t duty) {
    if (duty > LEDC_MAX_DUTY) {
        duty = LEDC_MAX_DUTY;
    }

    // Handle active-low
    if (STATUS_LED_ACTIVE_LOW) {
        duty = LEDC_MAX_DUTY - duty;
    }

    if (duty == last_duty) {
        return;
    }

    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    last_duty = duty;
}

esp_err_t status_led_init(void) {
    // Configure LEDC timer
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer: %s", esp_err_to_name(err));
        return err;
    }

    // Configure LEDC channel
    ledc_channel_config_t channel_conf = {
        .gpio_num = STATUS_LED_GPIO,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = STATUS_LED_ACTIVE_LOW ? LEDC_MAX_DUTY : 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };
    err = ledc_channel_config(&channel_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC channel: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Status LED initialized on GPIO%d (active-%s)",
             STATUS_LED_GPIO, STATUS_LED_ACTIVE_LOW ? "low" : "high");
    return ESP_OK;
}

void status_led_set_state(led_state_t state, bool active) {
    set_state_flag(state, active);
}

void status_led_mark_ring(void) {
    ring_until_ms = millis() + RING_LED_DURATION_MS;
}

bool status_led_is_ringing(void) {
    uint32_t now = millis();
    return (int32_t)(ring_until_ms - now) > 0;
}

void status_led_update(void) {
    uint32_t now = millis();
    uint8_t duty = 0;

    // Priority: Ringing > AP mode > WiFi connecting > SIP error > SIP ok > RTSP active
    if (status_led_is_ringing()) {
        duty = triangle_wave(now, RING_PULSE_PERIOD_MS, DUTY_PULSE_MIN, DUTY_RING_MAX);
    }
    else if (get_state_flag(LED_STATE_AP_MODE)) {
        duty = double_blink(now);
    }
    else if (get_state_flag(LED_STATE_WIFI_CONNECTING)) {
        duty = blink(now, WIFI_BLINK_PERIOD_MS);
    }
    else if (get_state_flag(LED_STATE_SIP_ERROR)) {
        duty = triangle_wave(now, SIP_PULSE_PERIOD_MS, 0, DUTY_PULSE_MAX);
    }
    else if (get_state_flag(LED_STATE_SIP_OK)) {
        duty = DUTY_LOW;
    }
    else if (get_state_flag(LED_STATE_RTSP_ACTIVE)) {
        duty = rtsp_tick(now);
    }

    set_duty(duty);
}
